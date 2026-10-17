# STATUS

## 2026-10-16

### Confirmed

- XMEye private wrapper markers FC/FD/F9/FA identified on real recordings.
- A reference Python extractor successfully recovers an HEVC Annex-B stream.
- A known recording is HEVC despite its `.h264` extension.
- Remuxing with video stream copy proves payload can be preserved without re-encoding.
- VLC 3.0.24 is the initial Windows target.

### Repository

- Initial native demux project structure created.
- Reference parser copied from `pc-admin`.
- Native VLC module skeleton in progress.

### Next

1. Finish structural probe and payload codec detector in C.
2. Implement sequential FC/FD/F9/FA reader.
3. Feed HEVC frames directly to VLC ES/packetizer.
4. Compile `libxmeye_plugin.dll` against VLC 3.0.24.
5. Test against XMEye sample without altering the source recording.
6. Add G.711 A-law and additional XMEye variants as samples arrive.

### Read-only probe results

Three current private XMEye samples were scanned structurally without extraction or decoding. All three show the same FC/FD/F9 wrapper family, zero invalid records, and HEVC parameter sets in FC keyframes. This validates a shared first demux path for the current files.

### Build environment

The t520 currently has no native C toolchain (`gcc`, `clang`, `cl`, `cmake`, `meson`, `ninja` absent). Compilation should therefore use a controlled external/CI toolchain rather than burdening the thin client unnecessarily.

## 2026-10-17 — probe mid-GOP corregido

- Reproducido el fallo con `Local_*.h264`: VLC caía en el demux `ps` porque el probe XMEye de 256 KiB veía registros FD válidos pero no alcanzaba el primer FC con VPS/SPS/PPS.
- Primer FC observado en las tres muestras Local: 771678, 1034658 y 1282508 bytes.
- El probe es ahora adaptativo: 256 KiB normalmente y hasta 2 MiB sólo cuando la estructura ya parece XMEye pero falta confirmar el códec.
- Commit de corrección: `9ab59ff` (`Extend XMEye probe for mid-GOP exports`). CI Win64: OK.
- DLL nueva probada sólo en el VLC portable, con cache de plugins regenerada y copia de seguridad del demux anterior.
- Prueba sin salida visible (`vdummy`): las tres `Local_*.h264`, una `NVR1-out_*.h264` y una `4f34a*.h264` seleccionan `xmeye` y arrancan el decoder HEVC.
- La instalación normal de VLC no se ha modificado con esta corrección todavía.

## 2026-10-17 — integración XMEye validada

- Se da por **válida** la integración XMEye para VLC 3.0.24 x64.
- `libxmeye_plugin.dll` abre correctamente grabaciones XMEye nativas, incluidos `NVR1-out_*.h264`, `4f34a*.h264` y los `Local_*.h264` que empiezan a mitad de GOP.
- `libavi_plugin.dll` parcheado detecta los AVI XMEye etiquetados como H.264 pero con payload HEVC y elimina la cabecera privada de 8 bytes de los paquetes clave.
- En el ASUS, tras actualizar a VLC 3.0.24 x64 e instalar ambas DLL, NVR AVI, NVR nativo y `Local_*` reproducen imagen correctamente y **sin bloques grises**.
- El HP T520 reproduce NVR_01 AVI con fluidez, pero los `Local_*` 2592x1944/25 fps HEVC exceden su margen de proceso: el reproductor oficial XMEye también reduce notablemente los FPS. Se considera una limitación de rendimiento del T520, no un fallo del formato ni del demux.
- Baseline validado: `libxmeye_plugin.dll` commit `cd7af35` + compatibilidad AVI commit `8255dc7`.
- A partir de este punto, cambios de zoom/filtros/UI deben desarrollarse sin alterar esta baseline de demux validada salvo regresión demostrada.
