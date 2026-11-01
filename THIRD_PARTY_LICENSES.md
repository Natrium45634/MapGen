# Сторонние компоненты и лицензии

Программа работает офлайн: внешние сервисы и загрузка геоданных для генерации не нужны.
Все численные алгоритмы (SHA-256, PCG64, шум, сетка, все стадии генерации, запись GeoTIFF,
контейнер `.geoworld`) реализованы в этом репозитории.

| Компонент | Версия | Где используется | Лицензия | Источник |
|---|---|---|---|---|
| Qt (Core, Gui, Widgets, OpenGL, OpenGLWidgets) | 6.8.3 (сборка Windows), ≥ 6.4 (Linux) | оболочка, отрисовка, PNG, сжатие zlib, QSaveFile | LGPL-3.0 (динамическая компоновка, DLL поставляются рядом с exe и могут быть заменены пользователем) | https://www.qt.io/licensing/ , исходники: https://download.qt.io/archive/qt/ |
| zlib (в составе Qt) | из Qt | сжатие слоёв `.geoworld` (`qCompress`) | zlib License | https://zlib.net/zlib_license.html |
| libpng (в составе Qt) | из Qt | запись PNG | libpng License | http://www.libpng.org/pub/png/src/libpng-LICENSE.txt |
| Mesa llvmpipe (`opengl32sw.dll`, развёртывается `windeployqt`) | из поставки Qt | программный OpenGL на компьютерах без драйвера OpenGL 3.3 | MIT | https://docs.mesa3d.org/license.html |
| Microsoft Visual C++ Runtime (`msvcp140.dll`, `vcruntime140*.dll`) | VS 2022 (v143) | среда выполнения C++ | Microsoft Software License Terms (распространяемый компонент Visual Studio) | https://visualstudio.microsoft.com/license-terms/ |
| Catch2 | 3.4.0 | только тесты (не поставляется) | BSL-1.0 | https://github.com/catchorg/Catch2 |
| Inno Setup | 6.x | только сборка установщика (не поставляется) | Inno Setup License | https://jrsoftware.org/files/is/license.txt |

## Закрепление версий

* Qt: `QT_VERSION` в `.github/workflows/build.yml` (установка `jurplel/install-qt-action@v4`, aqtinstall).
* Catch2: тег `v3.4.0` в `tests/CMakeLists.txt` (или системный пакет той же мажорной версии).
* Компилятор: MSVC 2022 (v143), образ `windows-2022`; Linux — GCC 13 (`ubuntu-24.04`).

## Научные материалы

Публикации и наборы данных, перечисленные в `docs/ALGORITHMS.md` (USGS, NOAA, NASA, GEBCO, ETOPO,
HydroSHEDS, WorldClim, ERA5, Köppen–Geiger Beck et al.), использованы как источник
закономерностей и сводных диапазонов. Сами наборы данных не поставляются и не скачиваются
программой; права на их загрузку для сравнения проверяются отдельно (см. `docs/TESTING.md`).
