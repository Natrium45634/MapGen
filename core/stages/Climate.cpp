// Stage 4: climate — 12 monthly fields of temperature, precipitation, PET, winds,
// humidity, soil water, snow and ice.
//
// Model (parameterisations, documented in docs/ALGORITHMS.md):
//  * Daily-mean top-of-atmosphere insolation from latitude, month and axial tilt.
//  * Zonal energy-balance model (Budyko–Sellers: OLR = A + B·T, diffusive heat transport in
//    sin(latitude), ice–albedo feedback), solved implicitly; its transport term drives a per-cell
//    seasonal energy balance with land/ocean heat capacity (thermal inertia, continentality).
//  * Simplified ocean currents: warm western and cold eastern boundary anomalies of ocean basins
//    in the subtropics, warm eastern / cold western anomalies in the subpolar band (flagged
//    "упрощённая").
//  * Air temperature = semi-Lagrangian advection of air masses with relaxation to the local
//    surface balance (maritime vs continental climates downwind of oceans).
//  * Surface winds: Hadley/Ferrel/polar belts that follow the seasonal thermal equator (ITCZ),
//    plus thermally driven pressure anomalies over land (monsoons), Coriolis-deflected.
//  * Moisture: precipitable water advected by the wind; ocean evaporation and land recycling;
//    condensation by large-scale saturation, convergence (ITCZ), frontal activity, orographic
//    lifting (windward rain) with leeward descent suppression (rain shadows); supersaturation.
//  * Full grid: lapse rate depending on humidity, sub-grid orographic precipitation,
//    Hargreaves PET, soil water bucket, degree-day snow, glaciers, ice sheets (plastic profile),
//    sea ice, ice shelves, cold-epoch (LGM-like) glaciation scenario for glacial landforms.
// Iterations: up to 3 controlled passes for the ice–albedo feedback with a convergence criterion.

#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/rng/Noise.h"
#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

constexpr double kS0 = 1361.0;
constexpr double kA = 203.3;  // W m-2 (Budyko OLR intercept)
constexpr double kB = 2.09;   // W m-2 K-1
constexpr double kDiff = 0.72;  // W m-2 K-1, meridional heat transport coefficient (x = sin(lat))
constexpr double kCOcean = 9.0; // W yr m-2 K-1 (~70 m mixed layer)
constexpr double kCLand = 0.9;   // land + air column + exchange with surrounding air (effective)
constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

double insolation(double latRad, double solarLonRad, double tiltRad) {
    double dec = std::asin(std::sin(tiltRad) * std::sin(solarLonRad));
    double t = -std::tan(latRad) * std::tan(dec);
    double h0;
    if (t >= 1.0) h0 = 0.0;
    else if (t <= -1.0) h0 = kPi;
    else h0 = std::acos(t);
    return kS0 / kPi * (h0 * std::sin(latRad) * std::sin(dec) + std::cos(latRad) * std::cos(dec) * std::sin(h0));
}

double solarLongitudeForMonth(int m) {
    // Mid-month day of year; December solstice (λ = 270°) at day 355.
    static const double midDay[12] = {15.5, 45, 74.5, 105, 135.5, 166, 196.5, 227.5, 258, 288.5, 319, 349.5};
    return (270.0 + (midDay[m] - 355.0) / 365.25 * 360.0) * kDegToRad;
}

double wsat(double tC) { return 15.0 * std::exp(0.06 * std::clamp(tC, -60.0, 45.0)); }  // saturated column water, mm

struct CGrid {
    const SphereGrid& g;
    int W, H;
    explicit CGrid(const SphereGrid& gg) : g(gg), W(gg.width()), H(gg.height()) {}
    int64_t id(int i, int j) const { return g.index(g.wrapCol(i), std::clamp(j, 0, H - 1)); }
};

// Separable box blur on the sphere grid (x-radius widened toward the poles), applied `passes` times.
void blurSphere(const SphereGrid& g, std::vector<double>& f, double radiusKm, int passes) {
    const int W = g.width(), H = g.height();
    std::vector<double> tmp(f.size());
    for (int p = 0; p < passes; ++p) {
        for (int j = 0; j < H; ++j) {
            int r = std::clamp(static_cast<int>(radiusKm / std::max(1.0, g.dxKm(j))), 0, W / 3);
            for (int i = 0; i < W; ++i) {
                double s = 0;
                for (int d = -r; d <= r; ++d) s += f[static_cast<size_t>(g.index(g.wrapCol(i + d), j))];
                tmp[static_cast<size_t>(g.index(i, j))] = s / (2 * r + 1);
            }
        }
        int ry = std::max(0, static_cast<int>(radiusKm / g.dyKm()));
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                double s = 0;
                int n = 0;
                for (int d = -ry; d <= ry; ++d) {
                    int jj = j + d;
                    if (jj < 0 || jj >= H) continue;
                    s += tmp[static_cast<size_t>(g.index(i, jj))];
                    ++n;
                }
                f[static_cast<size_t>(g.index(i, j))] = s / std::max(1, n);
            }
    }
}

// Semi-Lagrangian advection of a scalar field on the climate grid by (u, v) [m/s] over dt [s].
void advect(const SphereGrid& g, const std::vector<double>& src, const std::vector<float>& u, const std::vector<float>& v,
            double dt, std::vector<double>& dst, ThreadPool& pool) {
    const int W = g.width(), H = g.height();
    const double R = g.radiusKm() * 1000.0;
    pool.forRows(H, [&](int j) {
        double cl = std::max(0.05, g.cosLat(j));
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double dlon = -u[static_cast<size_t>(k)] * dt / (R * cl);  // radians
            double dlat = -v[static_cast<size_t>(k)] * dt / R;
            double x = i + 0.5 + dlon / g.dLonRad();
            double y = j + 0.5 - dlat / g.dLatRad();
            if (y < 0) { y = -y; x += W / 2.0; }
            if (y > H) { y = 2 * H - y; x += W / 2.0; }
            dst[static_cast<size_t>(k)] = g.sampleBilinear(src.data(), x, y);
        }
    });
}

}  // namespace

int climateGridWidth(int fullWidth);

void runClimate(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const SphereGrid& cg = w.climateGrid;
    const int W = g.width(), H = g.height(), CW = cg.width(), CH = cg.height();
    const int64_t N = g.size(), CN = cg.size();
    const double tilt = P.num("climate.axialTilt") * kDegToRad;
    const double dTgh = P.num("climate.temperatureOffset");
    const double seasonality = P.num("climate.seasonality");
    const double oroStrength = P.num("climate.orographic");
    const double humidity = P.num("climate.humidity");
    const double baseLapse = P.num("climate.lapseRate");
    const double sea = w.seaLevel();
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const float* elev = w.f32(LayerId::Elevation);
    CGrid C(cg);

    // ---------------------------------------------------------------- downsample geography
    std::vector<float> landFull(static_cast<size_t>(N)), hFull(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) {
        landFull[static_cast<size_t>(k)] = ocean[k] ? 0.0f : 1.0f;
        hFull[static_cast<size_t>(k)] = ocean[k] ? 0.0f : static_cast<float>(std::max(0.0, elev[k] - sea));
    }
    std::vector<float> landC(static_cast<size_t>(CN)), hC(static_cast<size_t>(CN));
    downsampleMean(g, landFull.data(), cg, landC.data());
    downsampleMean(g, hFull.data(), cg, hC.data());  // mean surface height incl. ocean zeros
    std::vector<double> hLand(static_cast<size_t>(CN));
    for (int64_t c = 0; c < CN; ++c) hLand[static_cast<size_t>(c)] = landC[static_cast<size_t>(c)] > 0.01 ? hC[static_cast<size_t>(c)] / landC[static_cast<size_t>(c)] : 0.0;
    // Smoothed orography for the large-scale flow (barrier height).
    std::vector<double> hSmooth(hC.begin(), hC.end());
    blurSphere(cg, hSmooth, 120.0, 1);

    // ---------------------------------------------------------------- ocean currents (simplified)
    std::vector<double> sstA(static_cast<size_t>(CN), 0.0);
    for (int j = 0; j < CH; ++j) {
        double lat = cg.latDeg(j), al = std::fabs(lat);
        double subtrop = gauss(al - 28.0, 12.0), upwell = gauss(al - 22.0, 9.0), subpolar = gauss(al - 56.0, 9.0);
        double dx = cg.dxKm(j);
        bool anyLand = false;
        for (int i = 0; i < CW; ++i) anyLand |= landC[static_cast<size_t>(cg.index(i, j))] > 0.5f;
        if (!anyLand) continue;  // circumpolar ocean: no boundary currents
        for (int i = 0; i < CW; ++i) {
            int64_t c = cg.index(i, j);
            if (landC[static_cast<size_t>(c)] > 0.5f) continue;
            int sw = 0, se = 0;
            while (sw < CW && landC[static_cast<size_t>(cg.index(cg.wrapCol(i - sw - 1), j))] <= 0.5f) ++sw;
            while (se < CW && landC[static_cast<size_t>(cg.index(cg.wrapCol(i + se + 1), j))] <= 0.5f) ++se;
            double dW = (sw + 0.5) * dx, dE = (se + 0.5) * dx, width = dW + dE;
            if (width < 2000) continue;
            double a = 0;
            a += 4.5 * subtrop * std::exp(-dW / 900.0);   // warm western boundary current
            a -= 5.0 * upwell * std::exp(-dE / 650.0);    // cold eastern boundary upwelling
            a += 5.0 * subpolar * std::exp(-dE / 1800.0); // warm drift towards the eastern subpolar basin
            a -= 3.0 * subpolar * std::exp(-dW / 900.0);  // cold western subpolar current
            sstA[static_cast<size_t>(c)] = a * std::min(1.0, width / 5000.0);
        }
    }
    blurSphere(cg, sstA, 250.0, 1);
    for (int64_t c = 0; c < CN; ++c) if (landC[static_cast<size_t>(c)] > 0.5f) sstA[static_cast<size_t>(c)] = 0;

    // ---------------------------------------------------------------- zonal EBM (implicit) -> transport term
    const int NB = 90;
    std::vector<double> xb(NB), latB(NB), landZ(NB, 0.0);
    for (int b = 0; b < NB; ++b) {
        latB[static_cast<size_t>(b)] = std::asin(-1.0 + (b + 0.5) * 2.0 / NB);
        xb[static_cast<size_t>(b)] = std::sin(latB[static_cast<size_t>(b)]);
    }
    {
        std::vector<double> cnt(NB, 0.0);
        for (int j = 0; j < CH; ++j) {
            int b = std::clamp(static_cast<int>((std::sin(cg.latRad(j)) + 1.0) / 2.0 * NB), 0, NB - 1);
            for (int i = 0; i < CW; ++i) { landZ[static_cast<size_t>(b)] += landC[static_cast<size_t>(cg.index(i, j))]; cnt[static_cast<size_t>(b)] += 1; }
        }
        for (int b = 0; b < NB; ++b) landZ[static_cast<size_t>(b)] = cnt[static_cast<size_t>(b)] > 0 ? landZ[static_cast<size_t>(b)] / cnt[static_cast<size_t>(b)] : 0.3;
    }
    const int stepsPerYear = 96;
    const double dtYr = 1.0 / stepsPerYear;
    auto albedoOf = [](double x, double tC, double ice, double land = 0.0) {
        double a = 0.31 + 0.11 * 0.5 * (3 * x * x - 1);  // zenith-angle dependence (Legendre P2)
        // Seasonal snow on land is partly masked by vegetation (≤ 0.5); sea ice and ice sheets are brighter.
        double snowAlb = lerp(0.62, 0.5, land);
        double frozen = smoothstep(-2.0, -15.0, tC);
        double a1 = lerp(a, snowAlb, frozen);
        return lerp(a1, 0.62, ice);
    };
    // Transport H[month][band] (W m-2), averaged over the substeps of each month.
    std::vector<double> Tz(NB, 10.0), transport(static_cast<size_t>(12 * NB), 0.0), Tzm(static_cast<size_t>(12 * NB), 0.0);
    {
        const double dx = 2.0 / NB;
        std::vector<double> a(NB), bdiag(NB), cdiag(NB), rhs(NB);
        for (int year = 0; year < 12; ++year) {
            std::fill(transport.begin(), transport.end(), 0.0);
            std::fill(Tzm.begin(), Tzm.end(), 0.0);
            for (int s = 0; s < stepsPerYear; ++s) {
                int m = s * 12 / stepsPerYear;
                double lam = solarLongitudeForMonth(m) + ((s % (stepsPerYear / 12)) - 3.5) / (stepsPerYear / 12) * kTwoPi / 12.0;
                for (int b = 0; b < NB; ++b) {
                    double cap = landZ[static_cast<size_t>(b)] * kCLand / seasonality + (1 - landZ[static_cast<size_t>(b)]) * kCOcean;
                    double q = insolation(latB[static_cast<size_t>(b)], lam, tilt) * (1.0 - albedoOf(xb[static_cast<size_t>(b)], Tz[static_cast<size_t>(b)], 0.0, landZ[static_cast<size_t>(b)]));
                    double xm = b > 0 ? 0.5 * (xb[static_cast<size_t>(b)] + xb[static_cast<size_t>(b) - 1]) : -1.0;
                    double xp = b < NB - 1 ? 0.5 * (xb[static_cast<size_t>(b)] + xb[static_cast<size_t>(b) + 1]) : 1.0;
                    double km = b > 0 ? kDiff * (1 - xm * xm) / (dx * dx) : 0.0;
                    double kp = b < NB - 1 ? kDiff * (1 - xp * xp) / (dx * dx) : 0.0;
                    a[static_cast<size_t>(b)] = -km;
                    cdiag[static_cast<size_t>(b)] = -kp;
                    bdiag[static_cast<size_t>(b)] = cap / dtYr + kB + km + kp;
                    rhs[static_cast<size_t>(b)] = cap / dtYr * Tz[static_cast<size_t>(b)] + q - (kA - kB * dTgh);
                }
                // Thomas algorithm.
                for (int b = 1; b < NB; ++b) {
                    double mlt = a[static_cast<size_t>(b)] / bdiag[static_cast<size_t>(b) - 1];
                    bdiag[static_cast<size_t>(b)] -= mlt * cdiag[static_cast<size_t>(b) - 1];
                    rhs[static_cast<size_t>(b)] -= mlt * rhs[static_cast<size_t>(b) - 1];
                }
                std::vector<double> Tn(NB);
                Tn[NB - 1] = rhs[NB - 1] / bdiag[NB - 1];
                for (int b = NB - 2; b >= 0; --b)
                    Tn[static_cast<size_t>(b)] = (rhs[static_cast<size_t>(b)] - cdiag[static_cast<size_t>(b)] * Tn[static_cast<size_t>(b) + 1]) / bdiag[static_cast<size_t>(b)];
                for (int b = 0; b < NB; ++b) {
                    double xm = b > 0 ? 0.5 * (xb[static_cast<size_t>(b)] + xb[static_cast<size_t>(b) - 1]) : -1.0;
                    double xp = b < NB - 1 ? 0.5 * (xb[static_cast<size_t>(b)] + xb[static_cast<size_t>(b) + 1]) : 1.0;
                    double fm = b > 0 ? kDiff * (1 - xm * xm) * (Tn[static_cast<size_t>(b)] - Tn[static_cast<size_t>(b) - 1]) / dx : 0.0;
                    double fp = b < NB - 1 ? kDiff * (1 - xp * xp) * (Tn[static_cast<size_t>(b) + 1] - Tn[static_cast<size_t>(b)]) / dx : 0.0;
                    transport[static_cast<size_t>(m * NB + b)] += (fp - fm) / dx / (stepsPerYear / 12);
                    Tzm[static_cast<size_t>(m * NB + b)] += Tn[static_cast<size_t>(b)] / (stepsPerYear / 12);
                }
                Tz = Tn;
            }
        }
    }
    {
        std::string dbg = "Зональная ЭБМ (год): ";
        for (int b = 0; b < NB; b += 9) {
            double t = 0;
            for (int m = 0; m < 12; ++m) t += Tzm[static_cast<size_t>(m * NB + b)] / 12;
            dbg += formatNumber(std::round(latB[static_cast<size_t>(b)] * kRadToDeg)) + "°:" + formatNumber(std::round(t)) + " ";
        }
        ctx.report.log(dbg);
    }
    ctx.progress(0.12, "энергобаланс");

    // ---------------------------------------------------------------- iterated climate on the coarse grid
    std::vector<std::vector<float>> Ts(12, std::vector<float>(static_cast<size_t>(CN)));   // sea-level air temperature
    std::vector<std::vector<float>> Uw(12, std::vector<float>(static_cast<size_t>(CN))), Vw(12, std::vector<float>(static_cast<size_t>(CN)));
    std::vector<std::vector<float>> Pc(12, std::vector<float>(static_cast<size_t>(CN)));   // mm/day
    std::vector<std::vector<float>> RHc(12, std::vector<float>(static_cast<size_t>(CN)));
    std::vector<double> iceCover(static_cast<size_t>(CN), 0.0);  // diagnosed snow/ice fraction (albedo feedback)
    std::vector<double> lapseC(static_cast<size_t>(CN), baseLapse);
    // Climate-grid soil water store (mm): land evaporation draws on water stored in wet months.
    std::vector<double> soilC(static_cast<size_t>(CN), 60.0);
    double evapScale = 1.3;  // evaporation coefficient, re-calibrated between iterations
    int iterations = 0;
    double lastMeanT = 1e9, meanT = 0;
    bool converged = false;

    for (int outer = 0; outer < 3; ++outer) {
        ++iterations;
        // ---- per-cell seasonal energy balance (surface equilibrium temperature at sea level)
        std::vector<std::vector<double>> Tloc(12, std::vector<double>(static_cast<size_t>(CN)));
        std::vector<double> insTab(static_cast<size_t>(CH) * stepsPerYear);
        for (int j = 0; j < CH; ++j)
            for (int s = 0; s < stepsPerYear; ++s) {
                int m = s * 12 / stepsPerYear;
                double lam = solarLongitudeForMonth(m) + ((s % (stepsPerYear / 12)) - 3.5) / (stepsPerYear / 12) * kTwoPi / 12.0;
                insTab[static_cast<size_t>(j) * stepsPerYear + s] = insolation(cg.latRad(j), lam, tilt);
            }
        // Zonal transport interpolated between EBM bands (no steps between climate-grid rows).
        std::vector<double> trRow(static_cast<size_t>(CH) * 12), t0Row(static_cast<size_t>(CH));
        for (int j = 0; j < CH; ++j) {
            double fb = (std::sin(cg.latRad(j)) + 1.0) / 2.0 * NB - 0.5;
            int b0 = std::clamp(static_cast<int>(std::floor(fb)), 0, NB - 1), b1 = std::min(NB - 1, b0 + 1);
            double t = std::clamp(fb - b0, 0.0, 1.0);
            for (int m = 0; m < 12; ++m)
                trRow[static_cast<size_t>(j) * 12 + m] = lerp(transport[static_cast<size_t>(m * NB + b0)], transport[static_cast<size_t>(m * NB + b1)], t);
            t0Row[static_cast<size_t>(j)] = lerp(Tzm[static_cast<size_t>(b0)], Tzm[static_cast<size_t>(b1)], t);
        }
        ctx.pool.forRows(CH, [&](int j) {
            double lat = cg.latRad(j);
            double x = std::sin(lat);
            for (int i = 0; i < CW; ++i) {
                int64_t c = cg.index(i, j);
                double lf = landC[static_cast<size_t>(c)];
                double cap = lf * kCLand / seasonality + (1 - lf) * kCOcean;
                double T = t0Row[static_cast<size_t>(j)];
                std::vector<double> acc(12, 0.0);
                for (int year = 0; year < 4; ++year)
                    for (int s = 0; s < stepsPerYear; ++s) {
                        int m = s * 12 / stepsPerYear;
                        // Albedo from the sea-level reference temperature (seasonal snow / sea ice) plus the diagnosed
                        // perennial land ice of the previous iteration. Elevation cooling is applied afterwards by the
                        // lapse rate, so high plateaus do not drive a spurious snowball of the reference level.
                        double alb = albedoOf(x, T, iceCover[static_cast<size_t>(c)], lf);
                        double q = insTab[static_cast<size_t>(j) * stepsPerYear + s] * (1.0 - alb);
                        // Freezing sea: once the surface reaches −1.8 °C, sea ice insulates the water and the
                        // effective heat capacity collapses (the air above ice can get much colder).
                        double capEff = lf < 0.5 ? lerp(cap, 2.5, smoothstep(-1.0, -12.0, T)) : cap;
                        double dT = (q - (kA - kB * dTgh) - kB * T + trRow[static_cast<size_t>(j) * 12 + m]) / capEff * dtYr;
                        T += dT;
                        if (year == 3) acc[static_cast<size_t>(m)] += T / (stepsPerYear / 12);
                    }
                for (int m = 0; m < 12; ++m) Tloc[static_cast<size_t>(m)][static_cast<size_t>(c)] = acc[static_cast<size_t>(m)] + (1 - lf) * sstA[static_cast<size_t>(c)];
            }
        }, &ctx.cancel);
        // Light meridional smoothing removes row-to-row bistability of the sea-ice threshold.
        for (int m = 0; m < 12; ++m) {
            auto& T = Tloc[static_cast<size_t>(m)];
            std::vector<double> sm(T.size());
            for (int j = 0; j < CH; ++j)
                for (int i = 0; i < CW; ++i) {
                    double a = T[static_cast<size_t>(C.id(i, j - 1))], b = T[static_cast<size_t>(C.id(i, j))], d = T[static_cast<size_t>(C.id(i, j + 1))];
                    sm[static_cast<size_t>(cg.index(i, j))] = 0.25 * a + 0.5 * b + 0.25 * d;
                }
            T.swap(sm);
        }
        {
            double s0 = 0, s1 = 0, aa = 0;
            for (int j = 0; j < CH; ++j)
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    double a = cg.cellAreaKm2(j);
                    for (int m = 0; m < 12; ++m) s0 += a * Tloc[static_cast<size_t>(m)][static_cast<size_t>(c)] / 12;
                    s1 += a * iceCover[static_cast<size_t>(c)];
                    aa += a;
                }
            ctx.report.log("итерация " + std::to_string(outer) + ": средняя локальная T " + formatNumber(std::round(s0 / aa * 10) / 10) +
                           ", доля льда " + formatNumber(std::round(s1 / aa * 100) / 100));
        }
        ctx.progress(0.2 + 0.2 * outer, "сезонный энергобаланс");

        // ---- winds per month (belts following the thermal equator + thermal pressure anomalies)
        for (int m = 0; m < 12; ++m) {
            const auto& T = Tloc[static_cast<size_t>(m)];
            // Thermal equator per column (smoothed in longitude).
            std::vector<double> itcz(static_cast<size_t>(CW)), colLand(static_cast<size_t>(CW), 0.0);
            for (int i = 0; i < CW; ++i) {
                double best = -1e9;
                int bj = CH / 2;
                for (int j = 0; j < CH; ++j) {
                    double lat = cg.latDeg(j);
                    if (std::fabs(lat) > 28) continue;
                    double v = T[static_cast<size_t>(cg.index(i, j))] - 0.02 * lat * lat;
                    if (v > best) { best = v; bj = j; }
                    if (std::fabs(lat) < 25) colLand[static_cast<size_t>(i)] += landC[static_cast<size_t>(cg.index(i, j))];
                }
                itcz[static_cast<size_t>(i)] = cg.latDeg(bj);
            }
            std::vector<double> itczS(static_cast<size_t>(CW));
            int r = std::max(1, CW / 18);
            for (int i = 0; i < CW; ++i) {
                double s = 0, l = 0;
                for (int d = -r; d <= r; ++d) { s += itcz[static_cast<size_t>(cg.wrapCol(i + d))]; l += colLand[static_cast<size_t>(cg.wrapCol(i + d))]; }
                s /= (2 * r + 1);
                double cap = l / (2 * r + 1) / std::max(1, CH / 4) > 0.25 ? 22.0 : 12.0;
                itczS[static_cast<size_t>(i)] = std::clamp(s, -cap, cap);
            }
            // Thermal pressure anomaly (heat low / cold high) over land.
            std::vector<double> zonalMean(static_cast<size_t>(CH), 0.0);
            for (int j = 0; j < CH; ++j) {
                double s = 0;
                for (int i = 0; i < CW; ++i) s += T[static_cast<size_t>(cg.index(i, j))];
                zonalMean[static_cast<size_t>(j)] = s / CW;
            }
            std::vector<double> pa(static_cast<size_t>(CN));
            for (int j = 0; j < CH; ++j)
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    pa[static_cast<size_t>(c)] = -(T[static_cast<size_t>(c)] - zonalMean[static_cast<size_t>(j)]) * landC[static_cast<size_t>(c)];
                }
            blurSphere(cg, pa, 700.0, 2);
            ctx.pool.forRows(CH, [&](int j) {
                double lat = cg.latDeg(j);
                double dy = cg.dyKm() * 1000.0;
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    double phiI = itczS[static_cast<size_t>(i)];
                    double hN = 30.0 + 0.45 * phiI, hS = -30.0 + 0.45 * phiI;
                    double pN = 60.0 + 0.3 * phiI, pS = -60.0 + 0.3 * phiI;
                    double u = 0, v = 0;
                    if (lat >= phiI && lat < hN) {
                        double t = (lat - phiI) / (hN - phiI);
                        u = -6.5 * std::sin(kPi * std::min(1.0, t * 1.15));
                        v = -2.6 * std::sin(kPi * t);
                    } else if (lat < phiI && lat > hS) {
                        double t = (phiI - lat) / (phiI - hS);
                        u = -6.5 * std::sin(kPi * std::min(1.0, t * 1.15));
                        v = 2.6 * std::sin(kPi * t);
                    } else if (lat >= hN && lat < pN) {
                        double t = (lat - hN) / (pN - hN);
                        u = 8.5 * std::sin(kPi * t);
                        v = 1.0 * std::sin(kPi * t);
                    } else if (lat <= hS && lat > pS) {
                        double t = (hS - lat) / (hS - pS);
                        u = 8.5 * std::sin(kPi * t);
                        v = -1.0 * std::sin(kPi * t);
                    } else if (lat >= pN) {
                        double t = (lat - pN) / (90.0 - pN);
                        u = -3.0 * std::sin(kPi * t);
                        v = -1.0 * std::sin(kPi * t);
                    } else {
                        double t = (pS - lat) / (90.0 + pS);
                        u = -3.0 * std::sin(kPi * t);
                        v = 1.0 * std::sin(kPi * t);
                    }
                    // Cross-equatorial monsoon branch: between the equator and a displaced ITCZ the trades that
                    // crossed the equator are turned eastwards by the Coriolis force (south-westerly monsoon in NH summer).
                    if (std::fabs(phiI) > 4.0 && lat * phiI > 0 && std::fabs(lat) < std::fabs(phiI)) {
                        double t = std::fabs(lat) / std::fabs(phiI);
                        double strength = smoothstep(4.0, 16.0, std::fabs(phiI));
                        u = lerp(u, 5.5 * std::sin(kPi * t), strength);
                    }
                    // Monsoon: flow down the thermal pressure gradient, deflected by Coriolis.
                    double dx = std::max(1000.0, cg.dxKm(j) * 1000.0);
                    double gx = (pa[static_cast<size_t>(C.id(i + 1, j))] - pa[static_cast<size_t>(C.id(i - 1, j))]) / (2 * dx);
                    double gy = (pa[static_cast<size_t>(C.id(i, j - 1))] - pa[static_cast<size_t>(C.id(i, j + 1))]) / (2 * dy);
                    double au = -gx * 6.0e5, av = -gy * 6.0e5;  // m/s per (K/m)
                    double defl = (lat >= 0 ? -1.0 : 1.0) * 0.75 * smoothstep(3.0, 20.0, std::fabs(lat));  // to the right in NH
                    double ru = au * std::cos(defl) - av * std::sin(defl), rv = au * std::sin(defl) + av * std::cos(defl);
                    double mag = std::hypot(ru, rv);
                    if (mag > 7.0) { ru *= 7.0 / mag; rv *= 7.0 / mag; }
                    u += ru;
                    v += rv;
                    double block = std::exp(-hSmooth[static_cast<size_t>(c)] / 3500.0);
                    Uw[static_cast<size_t>(m)][static_cast<size_t>(c)] = static_cast<float>(u * block);
                    Vw[static_cast<size_t>(m)][static_cast<size_t>(c)] = static_cast<float>(v * block);
                }
            });
        }

        // ---- air temperature: advection with relaxation to the local balance (continentality)
        for (int m = 0; m < 12; ++m) {
            std::vector<double> theta(Tloc[static_cast<size_t>(m)].begin(), Tloc[static_cast<size_t>(m)].end()), tmp(static_cast<size_t>(CN));
            const double dt = 6 * 3600.0;
            for (int s = 0; s < 36; ++s) {
                advect(cg, theta, Uw[static_cast<size_t>(m)], Vw[static_cast<size_t>(m)], dt, tmp, ctx.pool);
                for (int64_t c = 0; c < CN; ++c) {
                    double lf = landC[static_cast<size_t>(c)];
                    double tau = lf > 0.5 ? 5.0 : 1.2;  // days
                    double rel = 1.0 - std::exp(-0.25 / tau);
                    theta[static_cast<size_t>(c)] = tmp[static_cast<size_t>(c)] + rel * (Tloc[static_cast<size_t>(m)][static_cast<size_t>(c)] - tmp[static_cast<size_t>(c)]);
                }
            }
            // Meridional heat transport is the zonal EBM's job: keep each latitude row's mean of the local
            // balance, so advection only redistributes heat zonally (maritime vs continental climates).
            for (int j = 0; j < CH; ++j) {
                double a = 0, b = 0;
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    a += Tloc[static_cast<size_t>(m)][static_cast<size_t>(c)];
                    b += theta[static_cast<size_t>(c)];
                }
                double bias = (a - b) / CW;
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    Ts[static_cast<size_t>(m)][static_cast<size_t>(c)] = static_cast<float>(theta[static_cast<size_t>(c)] + bias);
                }
            }
        }
        {
            double s0 = 0, aa = 0;
            for (int j = 0; j < CH; ++j)
                for (int i = 0; i < CW; ++i) {
                    double a = cg.cellAreaKm2(j);
                    for (int m = 0; m < 12; ++m) s0 += a * Ts[static_cast<size_t>(m)][static_cast<size_t>(cg.index(i, j))] / 12;
                    aa += a;
                }
            ctx.report.log("итерация " + std::to_string(outer) + ": средняя T воздуха " + formatNumber(std::round(s0 / aa * 10) / 10));
        }
        ctx.progress(0.25 + 0.2 * outer, "ветры и воздушные массы");

        // ---- moisture transport and precipitation
        std::vector<double> Wv(static_cast<size_t>(CN));
        for (int64_t c = 0; c < CN; ++c) Wv[static_cast<size_t>(c)] = 0.6 * wsat(Ts[0][static_cast<size_t>(c)]);
        std::vector<double> runningP(static_cast<size_t>(CN), 2.0);
        for (int pass = 0; pass < 13; ++pass) {  // pass 0 = spin-up with January
            int m = pass == 0 ? 0 : pass - 1;
            const auto& U = Uw[static_cast<size_t>(m)];
            const auto& V = Vw[static_cast<size_t>(m)];
            // Convergence, orographic lifting and baroclinicity on the coarse grid.
            std::vector<double> conv(static_cast<size_t>(CN)), oro(static_cast<size_t>(CN)), front(static_cast<size_t>(CN));
            for (int j = 0; j < CH; ++j) {
                double dx = std::max(2000.0, cg.dxKm(j) * 1000.0), dy = cg.dyKm() * 1000.0;
                double cl = std::max(0.05, cg.cosLat(j));
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    double dudx = (U[static_cast<size_t>(C.id(i + 1, j))] - U[static_cast<size_t>(C.id(i - 1, j))]) / (2 * dx);
                    int jn = std::max(0, j - 1), js = std::min(CH - 1, j + 1);
                    double vcN = V[static_cast<size_t>(C.id(i, jn))] * std::max(0.05, cg.cosLat(jn));
                    double vcS = V[static_cast<size_t>(C.id(i, js))] * std::max(0.05, cg.cosLat(js));
                    double dvdy = (vcN - vcS) / ((js - jn) * dy) / cl;
                    conv[static_cast<size_t>(c)] = -(dudx + dvdy);
                    double hx = (hSmooth[static_cast<size_t>(C.id(i + 1, j))] - hSmooth[static_cast<size_t>(C.id(i - 1, j))]) / (2 * dx);
                    double hy = (hSmooth[static_cast<size_t>(C.id(i, jn))] - hSmooth[static_cast<size_t>(C.id(i, js))]) / ((js - jn) * dy);
                    oro[static_cast<size_t>(c)] = U[static_cast<size_t>(c)] * hx + V[static_cast<size_t>(c)] * hy;  // m/s vertical
                    double ty = (Ts[static_cast<size_t>(m)][static_cast<size_t>(C.id(i, jn))] - Ts[static_cast<size_t>(m)][static_cast<size_t>(C.id(i, js))]) / ((js - jn) * dy) * 1e6;
                    front[static_cast<size_t>(c)] = std::fabs(ty);  // K per 1000 km
                }
            }
            const double dt = 6 * 3600.0, dtDay = 0.25;
            int steps = pass == 0 ? 48 : 52;
            std::vector<double> accP(static_cast<size_t>(CN), 0.0), accRH(static_cast<size_t>(CN), 0.0), tmp(static_cast<size_t>(CN));
            int accN = 0;
            for (int s = 0; s < steps; ++s) {
                advect(cg, Wv, U, V, dt, tmp, ctx.pool);
                // Eddy mixing of moisture (synoptic eddies are not resolved by the mean wind).
                {
                    std::vector<double> mixed(tmp.size());
                    for (int j = 0; j < CH; ++j)
                        for (int i = 0; i < CW; ++i) {
                            double c0 = tmp[static_cast<size_t>(cg.index(i, j))];
                            double n = tmp[static_cast<size_t>(C.id(i, j - 1))], so = tmp[static_cast<size_t>(C.id(i, j + 1))];
                            double e = tmp[static_cast<size_t>(C.id(i + 1, j))], wv = tmp[static_cast<size_t>(C.id(i - 1, j))];
                            mixed[static_cast<size_t>(cg.index(i, j))] = c0 + 0.12 * (n + so - 2 * c0) + 0.05 * (e + wv - 2 * c0);
                        }
                    tmp.swap(mixed);
                }
                // Flux form: moisture piles up where the low-level flow converges (ITCZ, monsoon troughs)
                // and thins out under divergence (subtropical highs): dW/dt = −W·∇·V.
                // The parameterised wind is not mass-balanced and diverges over land on average, which would
                // export moisture from continents (opposite to the observed P − E > 0 over land); there only a
                // reduced share of divergence dries the column.
                for (int64_t c = 0; c < CN; ++c) {
                    double cvv = conv[static_cast<size_t>(c)];
                    if (cvv < 0) cvv *= 1.0 - 0.75 * landC[static_cast<size_t>(c)];
                    tmp[static_cast<size_t>(c)] *= std::exp(std::clamp(0.4 * cvv * dt, -0.05, 0.05));
                }
                bool accumulate = s >= steps - 32;
                ctx.pool.forRows(CH, [&](int j) {
                    double al = std::fabs(cg.latDeg(j));
                    for (int i = 0; i < CW; ++i) {
                        int64_t c = cg.index(i, j);
                        double T = Ts[static_cast<size_t>(m)][static_cast<size_t>(c)];
                        double Tsurf = T - lapseC[static_cast<size_t>(c)] * hSmooth[static_cast<size_t>(c)] / 1000.0;
                        double ws = wsat(Tsurf);
                        double wv = std::max(0.0, tmp[static_cast<size_t>(c)]);
                        double rh = std::min(1.2, wv / ws);
                        double lf = landC[static_cast<size_t>(c)];
                        double speed = std::hypot(U[static_cast<size_t>(c)], V[static_cast<size_t>(c)]);
                        // Evaporation (mm/day): open ocean (none under perennial ice) and land recycling.
                        double frozen = smoothstep(-1.8, -8.0, T);
                        double eOcean = humidity * 7.0 * evapScale * (wsat(T) / wsat(25.0)) * (0.55 + 0.45 * std::min(2.0, speed / 7.0)) *
                                        std::max(0.0, 1.0 - 0.6 * std::min(1.0, rh)) * (1.0 - frozen);
                        double petRate = 0.16 * std::max(0.0, Tsurf + 6.0);
                        double& soil = soilC[static_cast<size_t>(c)];
                        double eLand = petRate * std::clamp(soil / 75.0, 0.0, 1.0);
                        double E = (1 - lf) * eOcean + lf * eLand;
                        // Condensation rates (per day).
                        // Condensation efficiency rises with relative humidity; lifting mechanisms multiply it.
                        double cv = conv[static_cast<size_t>(c)];
                        double wOro = oro[static_cast<size_t>(c)];
                        double stormLat = 50.0 - 6.0 * std::cos(kTwoPi * (m + 0.5) / 12.0) * (cg.latDeg(j) >= 0 ? 1 : -1);
                        double base = 0.22 * smoothstep(0.45, 1.0, rh);
                        double enh = 1.0 + 1.0 * std::clamp(cv / 3e-6, 0.0, 3.0) * smoothstep(-5.0, 15.0, T)
                                   + 2.2 * std::clamp(front[static_cast<size_t>(c)] / 6.0, 0.3, 2.0) * gauss(al - stormLat, 15.0)
                                   + oroStrength * 2.0 * std::clamp(wOro / 0.08, 0.0, 4.0)
                                   + 0.8 * lf * smoothstep(5.0, 25.0, Tsurf);
                        double rate = base * enh;
                        if (cv < 0) rate *= std::exp(cv / (lf > 0.5 ? 2.5e-6 : 1.5e-6));   // subsidence suppresses rain
                        if (wOro < 0) rate *= std::exp(oroStrength * wOro / 0.08);         // descending lee air dries
                        double p = wv * (1.0 - std::exp(-rate * dtDay));
                        double wn = wv - p + E * dtDay;
                        double excess = wn - 1.05 * ws;
                        if (excess > 0) { p += excess; wn -= excess; }
                        Wv[static_cast<size_t>(c)] = std::max(0.0, wn);
                        if (lf > 0.01) soil = std::clamp(soil + (p - eLand * dtDay) * 0.9, 0.0, 150.0);  // 10 % drains to runoff
                        if (accumulate) {
                            accP[static_cast<size_t>(c)] += p / dtDay;
                            accRH[static_cast<size_t>(c)] += std::min(1.0, rh);
                        }
                    }
                });
                if (accumulate) ++accN;
            }
            {
                std::vector<double> pAvg(static_cast<size_t>(CN));
                for (int64_t c = 0; c < CN; ++c) pAvg[static_cast<size_t>(c)] = accP[static_cast<size_t>(c)] / accN;
                blurSphere(cg, pAvg, 90.0, 1);
                for (int64_t c = 0; c < CN; ++c) accP[static_cast<size_t>(c)] = pAvg[static_cast<size_t>(c)] * accN;
            }
            for (int64_t c = 0; c < CN; ++c) {
                double pd = accP[static_cast<size_t>(c)] / accN;
                runningP[static_cast<size_t>(c)] = 0.5 * runningP[static_cast<size_t>(c)] + 0.5 * pd;
                if (pass > 0) {
                    Pc[static_cast<size_t>(m)][static_cast<size_t>(c)] = static_cast<float>(pd);
                    RHc[static_cast<size_t>(m)][static_cast<size_t>(c)] = static_cast<float>(100.0 * accRH[static_cast<size_t>(c)] / accN);
                }
            }
            ctx.check();
        }
        // Global calibration of the evaporation coefficient: annual mean precipitation ≈ 1000 mm × humidity.
        {
            double sum = 0, area = 0;
            for (int j = 0; j < CH; ++j)
                for (int i = 0; i < CW; ++i) {
                    int64_t c = cg.index(i, j);
                    double a = cg.cellAreaKm2(j), yr = 0;
                    for (int m = 0; m < 12; ++m) yr += Pc[static_cast<size_t>(m)][static_cast<size_t>(c)] * kDaysInMonth[m];
                    sum += a * yr;
                    area += a;
                }
            double meanP = sum / area;
            double target = 1000.0 * humidity;
            double scale = std::clamp(target / std::max(1.0, meanP), 0.4, 2.5);
            // Feed the calibration back into the evaporation coefficient so that humidity (not only the
            // precipitation totals) is consistent in the next iteration.
            evapScale = std::clamp(evapScale * std::pow(scale, 0.9), 0.5, 4.0);
            for (int m = 0; m < 12; ++m)
                for (auto& v : Pc[static_cast<size_t>(m)]) v = static_cast<float>(v * scale);
            ctx.report.metric("precipCalibrationScale" + std::to_string(outer), scale);
        }
        // ---- lapse rate from humidity and diagnosed snow/ice cover for the albedo feedback
        meanT = 0;
        double area = 0;
        for (int j = 0; j < CH; ++j)
            for (int i = 0; i < CW; ++i) {
                int64_t c = cg.index(i, j);
                double pAnn = 0, tAnn = 0, tMinM = 1e9;
                for (int m = 0; m < 12; ++m) {
                    pAnn += Pc[static_cast<size_t>(m)][static_cast<size_t>(c)] * kDaysInMonth[m];
                    double t = Ts[static_cast<size_t>(m)][static_cast<size_t>(c)];
                    tAnn += t / 12;
                    tMinM = std::min(tMinM, t);
                }
                double wet = smoothstep(150.0, 1200.0, pAnn);
                lapseC[static_cast<size_t>(c)] = std::clamp(baseLapse + 1.6 * (0.5 - wet), 4.5, 9.0);
                double tSurf = tAnn - lapseC[static_cast<size_t>(c)] * hLand[static_cast<size_t>(c)] / 1000.0 * landC[static_cast<size_t>(c)];
                double lf = landC[static_cast<size_t>(c)];
                // Perennial land ice (ice sheets / glaciers) of this climate cell; sea ice is already in the
                // temperature-dependent albedo. Damped update keeps the feedback controlled.
                double landIce = smoothstep(-6.0, -16.0, tSurf) * smoothstep(50.0, 250.0, pAnn);
                double next = std::clamp(lf * landIce, 0.0, 1.0) * 0.6;
                iceCover[static_cast<size_t>(c)] = 0.5 * (iceCover[static_cast<size_t>(c)] + next);
                meanT += tAnn * cg.cellAreaKm2(j);
                area += cg.cellAreaKm2(j);
            }
        meanT /= area;
        if (std::fabs(meanT - lastMeanT) < 0.3) { converged = true; break; }
        lastMeanT = meanT;
    }
    ctx.report.metric("climateIterations", iterations);
    ctx.report.check("climate.convergence", "Итерации лёд–альбедо сошлись (|ΔT| < 0,3 °C)", converged, iterations,
                     "сходимость не более чем за 3 итерации");
    ctx.progress(0.7, "осадки");

    // ---------------------------------------------------------------- full-grid products
    auto Ltm = w.makeLayer(LayerId::TempMonthly);
    auto Lpm = w.makeLayer(LayerId::PrecMonthly);
    auto Lem = w.makeLayer(LayerId::PetMonthly);
    auto Lrm = w.makeLayer(LayerId::RunoffMonthly);
    auto Lsm = w.makeLayer(LayerId::SoilMonthly);
    auto Lwm = w.makeLayer(LayerId::SweMonthly);
    auto Ltmean = w.makeLayer(LayerId::TempMean), Ltmin = w.makeLayer(LayerId::TempMin), Ltmax = w.makeLayer(LayerId::TempMax);
    auto Lpa = w.makeLayer(LayerId::PrecAnnual), Lea = w.makeLayer(LayerId::PetAnnual), Lai = w.makeLayer(LayerId::Aridity);
    auto Lwb = w.makeLayer(LayerId::WaterBalance), Lra = w.makeLayer(LayerId::RunoffAnnual), Lsmean = w.makeLayer(LayerId::SoilMean);
    auto Lsnow = w.makeLayer(LayerId::SnowMonths), Lice = w.makeLayer(LayerId::IceType), Lith = w.makeLayer(LayerId::IceThickness);
    auto Lsea = w.makeLayer(LayerId::SeaIceMonths), Llgm = w.makeLayer(LayerId::LgmIce), Ldry = w.makeLayer(LayerId::DryMonths);
    auto Lps = w.makeLayer(LayerId::PrecSeasonality), Llap = w.makeLayer(LayerId::LapseRate);
    int16_t* tm = Ltm->as<int16_t>();
    uint16_t* pm = Lpm->as<uint16_t>();
    uint16_t* em = Lem->as<uint16_t>();
    uint16_t* rm = Lrm->as<uint16_t>();
    uint16_t* sm = Lsm->as<uint16_t>();
    uint16_t* swm = Lwm->as<uint16_t>();
    float *tmean = Ltmean->as<float>(), *tmin = Ltmin->as<float>(), *tmax = Ltmax->as<float>(), *pa = Lpa->as<float>(),
          *ea = Lea->as<float>(), *ai = Lai->as<float>(), *wb = Lwb->as<float>(), *ra = Lra->as<float>(), *smean = Lsmean->as<float>(),
          *ith = Lith->as<float>(), *ps = Lps->as<float>(), *lap = Llap->as<float>();
    uint8_t *snowM = Lsnow->as<uint8_t>(), *iceT = Lice->as<uint8_t>(), *lgm = Llgm->as<uint8_t>(), *dry = Ldry->as<uint8_t>();
    uint16_t* seaIce = Lsea->as<uint16_t>();

    // Insolation (MJ m-2 day-1) by row and month for Hargreaves PET.
    std::vector<double> raRow(static_cast<size_t>(H) * 12);
    for (int j = 0; j < H; ++j)
        for (int m = 0; m < 12; ++m) raRow[static_cast<size_t>(j) * 12 + m] = insolation(g.latRad(j), solarLongitudeForMonth(m), tilt) * 0.0864;
    std::vector<double> lapseD(lapseC.begin(), lapseC.end());
    const double sx = static_cast<double>(CW) / W, sy = static_cast<double>(CH) / H;
    std::vector<uint8_t> perennial(static_cast<size_t>(N), 0);
    ctx.pool.forRows(H, [&](int j) {
        double dyM = g.dyKm() * 1000.0, dxM = std::max(g.dxKm(j), 0.25 * g.dyKm()) * 1000.0;
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double cx = (i + 0.5) * sx, cy = (j + 0.5) * sy;
            bool isOcean = ocean[k] != 0;
            double hs = isOcean ? 0.0 : std::max(0.0, elev[k] - sea);
            double lapse = cg.sampleBilinear(lapseD.data(), cx, cy);
            lap[k] = static_cast<float>(lapse);
            // Sub-grid orographic slope along the wind (fine minus coarse).
            double gx = 0, gy = 0;
            if (!isOcean) {
                auto hAt = [&](int64_t q) { return ocean[q] ? 0.0 : std::max(0.0, elev[q] - sea); };
                gx = (hAt(g.neighbor(i, j, 2)) - hAt(g.neighbor(i, j, 6))) / (2 * dxM);
                gy = (hAt(g.neighbor(i, j, 0)) - hAt(g.neighbor(i, j, 4))) / (2 * dyM);
            }
            double tSum = 0, tLo = 1e9, tHi = -1e9, pSum = 0, eSum = 0, pMax = 0, pMin = 1e9;
            double Tm[12], Pm[12], Em[12];
            for (int m = 0; m < 12; ++m) {
                double tsl = cg.sampleBilinear(Ts[static_cast<size_t>(m)].data(), cx, cy);
                double T = tsl - lapse * hs / 1000.0;
                double pd = cg.sampleBilinear(Pc[static_cast<size_t>(m)].data(), cx, cy);
                if (!isOcean) {
                    double u = cg.sampleBilinear(Uw[static_cast<size_t>(m)].data(), cx, cy), v = cg.sampleBilinear(Vw[static_cast<size_t>(m)].data(), cx, cy);
                    double sp = std::max(0.5, std::hypot(u, v));
                    double sFine = (u * gx + v * gy) / sp;  // slope along the wind
                    double f = std::clamp(1.0 + 25.0 * oroStrength * sFine, 0.35, 2.6);
                    pd *= f;
                }
                double days = kDaysInMonth[m];
                double P = std::max(0.0, pd * days);
                double dtr = 6.0 + 12.0 * std::exp(-P / 80.0);
                double pet = 0.0023 * 0.408 * raRow[static_cast<size_t>(j) * 12 + m] * std::max(0.0, T + 17.8) * std::sqrt(dtr) * days;
                Tm[m] = T;
                Pm[m] = P;
                Em[m] = pet;
                tm[m * N + k] = static_cast<int16_t>(std::lround(std::clamp(T, -320.0, 320.0) * 100.0));
                pm[m * N + k] = static_cast<uint16_t>(std::lround(std::clamp(P, 0.0, 6553.0) * 10.0));
                em[m * N + k] = static_cast<uint16_t>(std::lround(std::clamp(pet, 0.0, 6553.0) * 10.0));
                // Use the quantised values for aggregates so the monthly and annual layers agree exactly.
                T = tm[m * N + k] * 0.01;
                P = pm[m * N + k] * 0.1;
                pet = em[m * N + k] * 0.1;
                Tm[m] = T;
                Pm[m] = P;
                Em[m] = pet;
                tSum += T;
                tLo = std::min(tLo, T);
                tHi = std::max(tHi, T);
                pSum += P;
                eSum += pet;
                pMax = std::max(pMax, P);
                pMin = std::min(pMin, P);
            }
            tmean[k] = static_cast<float>(tSum / 12);
            tmin[k] = static_cast<float>(tLo);
            tmax[k] = static_cast<float>(tHi);
            pa[k] = static_cast<float>(pSum);
            ea[k] = static_cast<float>(eSum);
            ai[k] = static_cast<float>(eSum > 1.0 ? pSum / eSum : 10.0);
            wb[k] = static_cast<float>(pSum - eSum);
            double si = 0;
            for (int m = 0; m < 12; ++m) si += std::fabs(Pm[m] - pSum / 12);
            ps[k] = static_cast<float>(pSum > 1 ? std::min(1.0, si / pSum / 1.83) : 0.0);
            int nd = 0;
            for (int m = 0; m < 12; ++m) nd += Pm[m] < 0.5 * Em[m];
            dry[k] = static_cast<uint8_t>(nd);
            if (isOcean) {
                uint16_t bits = 0;
                for (int m = 0; m < 12; ++m) if (Tm[m] < -1.8) bits |= static_cast<uint16_t>(1u << m);
                seaIce[k] = bits;
                ra[k] = 0;
                smean[k] = notComputed();
                for (int m = 0; m < 12; ++m) { rm[m * N + k] = 0; sm[m * N + k] = 0; swm[m * N + k] = 0; }
                continue;
            }
            // Soil water bucket with degree-day snow (4 model years, the last one is kept).
            double slope = std::hypot(gx, gy);
            double smax = 150.0 * std::clamp(1.35 - 6.0 * slope, 0.45, 1.4);
            double quick = std::clamp(0.05 + 4.0 * slope, 0.05, 0.4);
            double S = 0.5 * smax, swe = 0, minSwe = 1e18, sweStartLast = 0, sumR = 0, sumS = 0;
            int snowMonths = 0;
            for (int year = 0; year < 4; ++year) {
                if (year == 3) sweStartLast = swe;
                for (int m = 0; m < 12; ++m) {
                    double T = Tm[m], Pv = Pm[m], pet = Em[m], days = kDaysInMonth[m];
                    double fs = std::clamp((1.0 - T) / 3.0, 0.0, 1.0);
                    swe += fs * Pv;
                    double melt = std::min(swe, 3.5 * std::max(0.0, T) * days);
                    swe -= melt;
                    double water = (1 - fs) * Pv + melt;
                    double fast = quick * water;
                    water -= fast;
                    double aet = std::min(S + water, pet * std::pow(std::clamp(S / smax, 0.0, 1.0), 0.7));
                    S += water - aet;
                    double runoff = fast + std::max(0.0, S - smax);
                    S = std::min(S, smax);
                    if (year == 3) {
                        rm[m * N + k] = static_cast<uint16_t>(std::lround(std::clamp(runoff, 0.0, 6553.0) * 10.0));
                        sm[m * N + k] = static_cast<uint16_t>(std::lround(std::clamp(S, 0.0, 6553.0) * 10.0));
                        swm[m * N + k] = static_cast<uint16_t>(std::lround(std::clamp(swe, 0.0, 65535.0)));
                        sumR += rm[m * N + k] * 0.1;
                        sumS += S;
                        if (swe > 5) ++snowMonths;
                        minSwe = std::min(minSwe, swe);
                    }
                }
            }
            ra[k] = static_cast<float>(sumR);
            smean[k] = static_cast<float>(sumS / 12);
            snowM[k] = static_cast<uint8_t>(snowMonths);
            perennial[static_cast<size_t>(k)] = (minSwe > 30.0 && swe >= sweStartLast) ? 1 : 0;
            // Cold-epoch scenario (−6 °C, −20 % precipitation): glaciation for glacial landforms / fjords.
            double sweL = 0, minL = 1e18;
            for (int year = 0; year < 3; ++year)
                for (int m = 0; m < 12; ++m) {
                    double T = Tm[m] - 6.0, Pv = Pm[m] * 0.8;
                    double fs = std::clamp((1.0 - T) / 3.0, 0.0, 1.0);
                    sweL += fs * Pv;
                    sweL -= std::min(sweL, 3.5 * std::max(0.0, T) * kDaysInMonth[m]);
                    if (year == 2) minL = std::min(minL, sweL);
                }
            lgm[k] = minL > 30.0 ? kIceSheet : kIceNone;
        }
    }, &ctx.cancel);
    ctx.progress(0.85, "почва и снег");

    // ---- glaciers, ice sheets (plastic profile), ice shelves
    {
        std::vector<int32_t> lab;
        int n = connectedComponents(g, perennial.data(), lab);
        std::vector<double> area(static_cast<size_t>(n), 0.0);
        for (int64_t k = 0; k < N; ++k) if (lab[static_cast<size_t>(k)] >= 0) area[static_cast<size_t>(lab[static_cast<size_t>(k)])] += g.cellAreaKm2(g.row(k));
        const double sheetArea = 150000.0 * sqr(g.radiusKm() / 6371.0);
        std::vector<uint8_t> sheet(static_cast<size_t>(N), 0);
        std::vector<int64_t> edge;
        for (int64_t k = 0; k < N; ++k) {
            int32_t c = lab[static_cast<size_t>(k)];
            if (c < 0) continue;
            bool isSheet = area[static_cast<size_t>(c)] >= sheetArea;
            iceT[k] = isSheet ? kIceSheet : kIceGlacier;
            ith[k] = isSheet ? 0.0f : 180.0f;
            if (isSheet) sheet[static_cast<size_t>(k)] = 1;
        }
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                int64_t k = g.index(i, j);
                if (!sheet[static_cast<size_t>(k)]) continue;
                bool e = false;
                for (int d = 0; d < 8 && !e; ++d) e = !sheet[static_cast<size_t>(g.neighbor(i, j, d))];
                if (e) edge.push_back(k);
            }
        std::vector<float> dEdge;
        distanceTransform(g, edge, dEdge, nullptr, 4000, sheet.data());
        const double tau0 = 60000.0, rho = 917.0, grav = 9.81;
        for (int64_t k = 0; k < N; ++k)
            if (sheet[static_cast<size_t>(k)]) ith[k] = static_cast<float>(std::min(4200.0, std::sqrt(2.0 * tau0 * (dEdge[static_cast<size_t>(k)] * 1000.0 + 5000.0) / (rho * grav))));
        // Ice shelves: shallow cold sea next to ice sheets.
        std::vector<float> dSheet;
        distanceTransform(g, edge, dSheet, nullptr, 400);
        for (int64_t k = 0; k < N; ++k)
            if (ocean[k] && dSheet[static_cast<size_t>(k)] < 220 && elev[k] - sea > -900 && tmean[k] < -9 && seaIce[k] == 0x0fff) iceT[k] = kIceShelf;
    }

    // ---- store
    auto putC = [&](LayerId id, const std::vector<std::vector<float>>& v) {
        auto l = w.makeLayer(id);
        for (int m = 0; m < 12; ++m) std::copy(v[static_cast<size_t>(m)].begin(), v[static_cast<size_t>(m)].end(), l->band<float>(m));
        w.setLayer(id, l);
    };
    putC(LayerId::WindU, Uw);
    putC(LayerId::WindV, Vw);
    putC(LayerId::RelHumidity, RHc);
    {
        auto l = w.makeLayer(LayerId::SstAnomaly);
        for (int64_t c = 0; c < CN; ++c) l->as<float>()[c] = static_cast<float>(sstA[static_cast<size_t>(c)]);
        w.setLayer(LayerId::SstAnomaly, l);
    }
    w.setLayer(LayerId::TempMonthly, Ltm);
    w.setLayer(LayerId::PrecMonthly, Lpm);
    w.setLayer(LayerId::PetMonthly, Lem);
    w.setLayer(LayerId::RunoffMonthly, Lrm);
    w.setLayer(LayerId::SoilMonthly, Lsm);
    w.setLayer(LayerId::SweMonthly, Lwm);
    w.setLayer(LayerId::TempMean, Ltmean);
    w.setLayer(LayerId::TempMin, Ltmin);
    w.setLayer(LayerId::TempMax, Ltmax);
    w.setLayer(LayerId::PrecAnnual, Lpa);
    w.setLayer(LayerId::PetAnnual, Lea);
    w.setLayer(LayerId::Aridity, Lai);
    w.setLayer(LayerId::WaterBalance, Lwb);
    w.setLayer(LayerId::RunoffAnnual, Lra);
    w.setLayer(LayerId::SoilMean, Lsmean);
    w.setLayer(LayerId::SnowMonths, Lsnow);
    w.setLayer(LayerId::IceType, Lice);
    w.setLayer(LayerId::IceThickness, Lith);
    w.setLayer(LayerId::SeaIceMonths, Lsea);
    w.setLayer(LayerId::LgmIce, Llgm);
    w.setLayer(LayerId::DryMonths, Ldry);
    w.setLayer(LayerId::PrecSeasonality, Lps);
    w.setLayer(LayerId::LapseRate, Llap);

    // ---- statistics and macro-pattern checks
    double aT = 0, aP = 0, area = 0, landArea = 0, landP = 0, iceArea = 0, seaIceArea = 0;
    double eqT = 0, eqA = 0, polT = 0, polA = 0, nhJul = 0, nhJan = 0, nhA = 0, shJul = 0, shJan = 0, shA = 0;
    double wetEq = 0, wetEqA = 0, subtrop = 0, subtropA = 0;
    for (int j = 0; j < H; ++j) {
        double a = g.cellAreaKm2(j), lat = g.latDeg(j);
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            aT += a * tmean[k];
            aP += a * pa[k];
            area += a;
            if (std::fabs(lat) < 10) { eqT += a * tmean[k]; eqA += a; }
            if (std::fabs(lat) > 70) { polT += a * tmean[k]; polA += a; }
            if (!ocean[k]) {
                landArea += a;
                landP += a * pa[k];
                if (iceT[k] == kIceSheet || iceT[k] == kIceGlacier) iceArea += a;
                if (lat > 35 && lat < 65) { nhJul += a * tm[6 * N + k] * 0.01; nhJan += a * tm[0 * N + k] * 0.01; nhA += a; }
                if (lat < -35 && lat > -65) { shJul += a * tm[6 * N + k] * 0.01; shJan += a * tm[0 * N + k] * 0.01; shA += a; }
                if (std::fabs(lat) < 8) { wetEq += a * pa[k]; wetEqA += a; }
                if (std::fabs(lat) > 18 && std::fabs(lat) < 32) { subtrop += a * pa[k]; subtropA += a; }
            } else if (seaIce[k] == 0x0fff) {
                seaIceArea += a;
            }
        }
    }
    {
        // UNEP aridity classes of land (Earth reference, CGIAR AI v3: ~7.5 / 12 / 18 / 10 / 52 %).
        double cls[5] = {0, 0, 0, 0, 0}, la = 0;
        for (int j = 0; j < H; ++j) {
            double a = g.cellAreaKm2(j);
            for (int i = 0; i < W; ++i) {
                int64_t k = g.index(i, j);
                if (ocean[k] || iceT[k] == kIceSheet) continue;
                double v = ai[k];
                int c = v < 0.05 ? 0 : v < 0.2 ? 1 : v < 0.5 ? 2 : v < 0.65 ? 3 : 4;
                cls[c] += a;
                la += a;
            }
        }
        const char* names[5] = {"landHyperarid", "landArid", "landSemiarid", "landDrySubhumid", "landHumid"};
        for (int c = 0; c < 5; ++c) ctx.report.metric(names[c], la > 0 ? cls[c] / la : 0);
    }
    double gT = aT / area;
    ctx.report.metric("globalMeanTempC", gT);
    ctx.report.metric("globalMeanPrecipMm", aP / area);
    ctx.report.metric("landMeanPrecipMm", landArea > 0 ? landP / landArea : 0);
    ctx.report.metric("equatorMeanTempC", eqA > 0 ? eqT / eqA : 0);
    ctx.report.metric("polarMeanTempC", polA > 0 ? polT / polA : 0);
    ctx.report.metric("landIceFraction", landArea > 0 ? iceArea / landArea : 0);
    ctx.report.metric("perennialSeaIceFractionOfSurface", seaIceArea / area);
    ctx.report.check("climate.gradient", "Экватор теплее полюсов", eqA == 0 || polA == 0 || eqT / eqA > polT / polA + 15, eqA > 0 ? eqT / eqA : 0,
                     "разница > 15 °C", true);
    if (nhA > 0 && tilt > 5 * kDegToRad)
        ctx.report.check("climate.seasonsNH", "Северное полушарие: июль теплее января (суша 35–65°)", nhJul / nhA > nhJan / nhA + 5,
                         (nhJul - nhJan) / nhA, "> 5 °C", true);
    if (shA > 0 && tilt > 5 * kDegToRad)
        ctx.report.check("climate.seasonsSH", "Южное полушарие: январь теплее июля (суша 35–65°)", shJan / shA > shJul / shA + 3,
                         (shJan - shJul) / shA, "> 3 °C", true);
    if (wetEqA > 0 && subtropA > 0)
        ctx.report.check("climate.wetTropics", "Экваториальная суша влажнее субтропической", wetEq / wetEqA > subtrop / subtropA,
                         wetEq / wetEqA - subtrop / subtropA, "> 0 мм/год");
    ctx.report.log("Океанические течения — упрощённая модель широтно-зависимых аномалий у западных и восточных границ океанов.");
    ctx.report.log("Средняя глобальная температура " + formatNumber(std::round(gT * 10) / 10) + " °C; годовые осадки калиброваны к 1000 мм × «общая влажность».");
    ctx.progress(1.0, "климат");
}

}  // namespace mapgen
