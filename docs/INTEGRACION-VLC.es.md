# Integración con VLC — documentación en español

**Proyecto:** [vlc-xmeye-demux](https://github.com/aibarz/vlc-xmeye-demux) · [English version](VLC-INTEGRATION.md)

## Modelo

Módulo nativo `demux` para VLC 3.x, no extensión Lua ni conversor externo.

```text
Archivo XMEye -> detección estructural -> registros FC/FD/F9/FA -> detector H.265/H.264 -> VLC ES -> decodificador
```

El módulo `libxmeye_plugin.dll` debe reconocer grabaciones XMEye por el contenido. Si no se valida la estructura y un payload plausible, tiene que devolver `VLC_EGENERIC` para permitir los demás demultiplexores de VLC.

## Instalación validada (solo objetivo inicial)

- VLC **3.0.24 x64** para Windows.
- `plugins\demux\libxmeye_plugin.dll` para las grabaciones nativas XMEye.
- `plugins\demux\libavi_plugin.dll` **parcheado y compatible** para los AVI XMEye mal etiquetados.
- Respaldar la DLL AVI original antes de reemplazarla y probar primero en VLC portable.
- Los binarios de una versión distinta de VLC pueden provocar errores o un VLC inutilizable.

## Consideraciones técnicas

- Preferir payload original sin recomprimir ni escribir temporalmente archivos MKV/MP4.
- Descartar registros de vídeo previos al primer fotograma clave utilizable.
- Sonda adaptativa para exportaciones que empiezan a mitad de GOP.
- Temporización procedente de marcas XMEye, con FPS configurable como respaldo.
- El rendimiento de decodificación HEVC es un problema distinto del demux: un procesador limitado puede descartar fotogramas.

## Privacidad

Nunca añadir grabaciones reales, seriales de DVR/NVR o capturas privadas como fixtures públicos. Emplear datos sintéticos para pruebas automatizadas.
