# vlc-xmeye-demux

**Repositorio / Repository:** https://github.com/aibarz/vlc-xmeye-demux

**[Español](#español) · [English](#english)**

## Español

**Demultiplexor nativo de VLC para grabaciones propietarias XMEye de DVR/NVR.** Permite abrir archivos XMEye directamente en VLC **sin transcodificación y sin generar un vídeo temporal**: elimina en memoria las cabeceras privadas y entrega el flujo comprimido original a los decodificadores habituales de VLC.

### Qué resuelve

- Grabaciones con marcadores privados `FC` (fotograma clave), `FD` (fotograma intermedio), `F9` (tiempo/control) y `FA` (audio G.711 A-law).
- Archivos con extensión `.h264` que contienen realmente vídeo **HEVC/H.265**; la detección se basa en el contenido, no en el nombre.
- Exportaciones `Local_*.h264` que empiezan a mitad de GOP; búsqueda adaptativa de cabeceras hasta 2 MiB.
- Exportaciones **AVI XMEye** etiquetadas erróneamente como H.264: requieren también el módulo AVI parcheado.
- Reconstrucción de cadencia a partir de marcas temporales; registros probados de 8 y 25 fps.

### Compatibilidad y estado

**Validado el 17/10/2026 en VLC 3.0.24 x64 para Windows**, con grabaciones XMEye reales. Se probaron conjuntamente `libxmeye_plugin.dll` y `libavi_plugin.dll` parcheado. Los archivos HEVC pesados pueden saturar equipos poco potentes (por ejemplo, HP T520); esto no implica necesariamente un fallo del demux.

**No es un plugin universal, ni una extensión Lua**, ni está garantizada su compatibilidad con otras versiones de VLC, firmwares XMEye o códecs no comprobados. La detección de H.264 forma parte del diseño; las muestras validadas hasta ahora son HEVC.

### Instalación experimental

1. Utilizar **VLC 3.0.24 x64** y hacer copia de seguridad de los módulos originales, especialmente `plugins\demux\libavi_plugin.dll`.
2. Obtener las DLL compiladas para esa versión mediante los flujos de [GitHub Actions](https://github.com/aibarz/vlc-xmeye-demux/actions), o compilarlas a partir del código. **Este repositorio no contiene DLL de vídeos privados**.
3. Copiar `libxmeye_plugin.dll` a `plugins\demux\`. Para AVI XMEye, instalar también el `libavi_plugin.dll` específicamente parcheado, tras guardar el original.
4. Regenerar la caché de módulos de VLC, si corresponde, y abrir una **copia** del archivo de prueba.

No sustituir módulos de una instalación de trabajo sin copia de seguridad ni mezclar versiones. Los archivos originales de las cámaras no deben modificarse.

### Estructura, documentación y licencia

- `src/xmeye.c`: módulo demux nativo; cabecera SPDX **LGPL-2.1-or-later**.
- `ci/` y `.github/workflows/`: compilación Win64 y compatibilidad del AVI XMEye.
- `tests/probe_xmeye.py`: detector estructural de solo lectura.
- `reference/Convert-Xmeye.py`: conversor de referencia (opcional, independiente del demux).
- [Formato XMEye — ES](docs/FORMATO.es.md) · [VLC — ES](docs/INTEGRACION-VLC.es.md) · [Estado técnico](STATUS.md) · [Créditos y terceros](THIRD_PARTY_NOTICES.md).

No se incluyen grabaciones reales, contraseñas ni material audiovisual de cámaras. El formato fue documentado a partir de muestras controladas y herramientas públicas, con crédito a Denis Victorov y su proyecto MIT [xmeye-dvr-utilities](https://github.com/polesse/xmeye-dvr-utilities). Las licencias y derechos de cada componente deben respetarse; este repositorio **no redistribuye VLC completo**.

---

## English

**Native VLC demultiplexer for proprietary XMEye DVR/NVR recordings.** Open XMEye recordings directly in VLC **without transcoding or temporary media files**. The plugin strips proprietary frame headers in memory and sends the original compressed elementary stream to VLC's standard decoder pipeline.

### Features

- Handles `FC` (key/I frame), `FD` (inter/P frame), `F9` (timestamp/control), and `FA` (G.711 A-law audio) records.
- Recognizes XMEye files named `.h264` whose actual payload is **HEVC/H.265**, by inspecting content rather than trusting the extension.
- Adaptive probe up to 2 MiB for `Local_*.h264` files starting in the middle of a GOP.
- **Mislabeled XMEye AVI exports** require the companion patched AVI module.
- Uses XMEye timing records to derive native playback rate; observed 8 fps and 25 fps.

### Compatibility and status

**Validated on 17 October 2026 with VLC 3.0.24 x64 on Windows** using real XMEye recordings and the matching `libxmeye_plugin.dll` + patched `libavi_plugin.dll` pair. High-resolution HEVC streams can exceed the processing capacity of low-powered machines (e.g. HP T520); slow playback on such hardware is not necessarily a demux format failure.

**This is not a universal VLC plugin or a Lua extension.** Other VLC versions, XMEye variations, and untested codecs are not guaranteed to work. The parser architecture allows H.264 detection, but the validated recordings currently use HEVC.

### Experimental installation

1. Use **VLC 3.0.24 x64**. Back up the original modules, especially `plugins\demux\libavi_plugin.dll`.
2. Build the required DLLs through [GitHub Actions](https://github.com/aibarz/vlc-xmeye-demux/actions), or compile the provided source. The repository does not embed proprietary video samples.
3. Place `libxmeye_plugin.dll` in `plugins\demux\`. For XMEye AVI files, install the matching patched `libavi_plugin.dll` after backing up the original.
4. Rebuild VLC's plugin cache if required and test on a **copy** of a recording.

Never replace production modules without a backup or mix incompatible VLC versions. Source recordings must remain untouched.

### Structure, docs and licensing

- `src/xmeye.c`: native VLC demux module; SPDX header **LGPL-2.1-or-later**.
- `ci/` and `.github/workflows/`: Win64 builds and AVI wrapper compatibility.
- `tests/probe_xmeye.py`: read-only frame structure probe.
- `reference/Convert-Xmeye.py`: optional reference converter, separate from the demux plugin.
- [Container format — EN](docs/FORMAT.md) · [VLC integration — EN](docs/VLC-INTEGRATION.md) · [Technical status](STATUS.md) · [Third-party acknowledgments](THIRD_PARTY_NOTICES.md).

No private camera recordings, passwords or personal visual/audio data are included. Related public work: Denis Victorov's MIT-licensed [xmeye-dvr-utilities](https://github.com/polesse/xmeye-dvr-utilities). Respect each component's license; **the VLC application itself is not redistributed**.

---

**Proyecto / Project:** [vlc-xmeye-demux](https://github.com/aibarz/vlc-xmeye-demux) · Public source, versioned development · Windows x64 target.
