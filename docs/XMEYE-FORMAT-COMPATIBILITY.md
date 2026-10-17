# XMEye / Xiongmai: catálogo de variantes y excepciones (VLC 3.0.24 x64)

Este registro reúne excepciones observadas y verificadas. No pretende afirmar compatibilidad con todas las versiones OEM o ficheros corruptos.

## Regla general

El demux y códec no se eligen a partir del nombre, la extensión, la etiqueta AVI FourCC o el prefijo 00 00 00 FD por sí solos. Una identificación incierta no autoriza a convertir H.264 en HEVC. Mantener la selección automática.

## Variantes observadas

| Variante | Firma/rasgo verificable | Tratamiento |
| --- | --- | --- |
| XMEye propio con extensión .h264 | Registros 00 00 01 FC / FD / F9 / FA | Demux propietario independiente y detección H.264/HEVC del contenido |
| AVI XMEye HEVC etiquetado H264 | Prefijo AVI 00 00 00 FD y cabeceras completas HEVC VPS 40 01, SPS 42 01, PPS 44 01 | Cambiar ES a HEVC y retirar cabecera privada si corresponde |
| AVI antiguo H.264 928x576 22 fps, prefijo privado | 00 00 00 FD ... 00 00 00 01 27 4D E0 ... (SPS H.264) | Conservar H.264; evitar el falso positivo HEVC tipo 19 |
| AVI H.264 convencional | RIFF AVI y H.264 SPS/PPS, sin firma HEVC inequívoca | Comportamiento del AVI estándar de VLC |
| AVI con firma ambigua o cabeceras incompletas | No hay triplete VPS+SPS+PPS verificable | Conservar códec declarado y registrar diagnóstico; no forzar HEVC |

## Por qué fallaba el AVI antiguo

La heurística anterior desplazaba un bit de la primera NAL después de 00 00 00 FD y aceptaba tipos HEVC 16..23. Una NAL H.264 con bytes 27 4D E0 coincidía falsamente con HEVC tipo 19. VLC cambiaba de decodificador, esperaba VPS/SPS/PPS inexistentes y dejaba de entregar imágenes. La corrección requiere cabeceras HEVC válidas y las tres NAL VPS/SPS/PPS antes de cambiar el ES.

## Variantes del demux propietario

- FC: fotograma clave, cabecera de 16 bytes, longitud de carga LE32 a +12.
- FD: delta, cabecera de 8 bytes, longitud de carga LE32 a +4.
- F9: metadatos/tiempo, 24 bytes.
- FA: audio G.711, cabecera de 8 bytes, longitud LE16 a +6.
- Puede empezar a mitad de GOP: saltar FD antes de un FC válido y ampliar sondeo inicial 256 KiB hasta 2 MiB cuando procede.
- Hay archivos de 8 y 25 fps; no inferir la frecuencia ni el códec de la extensión .h264.
- Distinguir las firmas FC/FD del prefijo AVI 00 00 00 FD.

## Validación y extensión

- Tests sintéticos: tests/test_avi_classifier.py.
- Comparación en ficheros reales: 75 paquetes de AVI H.264 antiguo, sin firmas HEVC completas; 75 de AVI NVR HEVC, firma completa en el primer paquete.
- Validación local de medios sin capturas ni extracción de fotogramas. No subir archivos, IP o nombres de cámaras a GitHub.
- Para cada formato nuevo: documentar firma estructural (RIFF/FourCC/NAL/longitud y flags), añadir test de regresión, comparar con los AVI antiguos y HEVC validados y no tocar el demux estable hasta las pruebas.

Una heurística no puede garantizar compatibilidad universal con variantes no observadas ni archivos dañados.