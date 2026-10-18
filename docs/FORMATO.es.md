# Formato de grabaciones XMEye — documentación en español

**Proyecto:** [vlc-xmeye-demux](https://github.com/aibarz/vlc-xmeye-demux) · [English version](FORMAT.md)

Este archivo describe lo observado en exportaciones de DVR/NVR XMEye; es ingeniería inversa sobre archivos propios y cotejo con utilidades de código abierto, **no una especificación oficial del fabricante**.

## Registros privados

Los registros conocidos comienzan por `00 00 01`; el cuarto byte indica el tipo.

| Tipo | Identificador | Cabecera / registro | Tamaño del contenido |
|---|---|---|---|
| Vídeo clave I | `FC` | 16 bytes antes del payload | LE32 en offset +12 |
| Vídeo P/intermedio | `FD` | 8 bytes antes del payload | LE32 en +4 |
| Audio A-law | `FA` | 8 bytes antes del payload | LE16 en +6 |
| Marca de tiempo/control | `F9` | 24 bytes completos | Sin contenido multimedia |

Para `FC`, la marca temporal LE32 conocida está en +8; el contenido empieza en +16. Para `FD`, en +8. Para `FA`, también en +8. En `F9`, se ha observado una marca temporal LE32 en +8.

## Códecs y extensiones

Una extensión `.h264` no determina el códec. Algunos archivos contienen **HEVC/H.265** con cabeceras Annex-B. Las NAL HEVC VPS/SPS/PPS incluyen tipos 32/33/34. En H.264 se esperan SPS/PPS/IDR de tipos 7/8/5. La capa contenedora `FC/FD/F9/FA` debe seguir desacoplada de los detectores de códec.

## Tiempo y sondas

La tasa de fotogramas se puede estimar con registros de tiempo y el número de imágenes transcurridas. La sonda inicial es normalmente 256 KiB; si hay estructura XMEye plausible pero falta el primer fotograma con parámetros de códec, puede extenderse hasta 2 MiB. No aceptar flujos no XMEye por una coincidencia aislada de bytes.

## Extensibilidad

Para nuevos DVR/firmwares/códecs, crear detectores adicionales y pruebas sobre muestras sintéticas o privadas **fuera del repositorio**, preservando el parser común cuando se mantenga este formato de registro.
