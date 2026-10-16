# BKV2 TileDelta

Formato little-endian diseñado para Viewer 2.x.

## Header (48 bytes)

| Offset | Tipo | Campo |
|---:|---|---|
| 0 | char[4] | `BKV2` |
| 4 | u16 | header size = 48 |
| 6 | u16 | width |
| 8 | u16 | height |
| 10 | u16 | fps numerator |
| 12 | u16 | fps denominator |
| 14 | u16 | flags (bit 0 = loop) |
| 16 | u16 | codec (1 = TileDelta RGB565) |
| 18 | u8 | block width |
| 19 | u8 | block height |
| 20 | u16 | nominal keyframe interval |
| 22 | u16 | reserved |
| 24 | u32 | frame count |
| 28 | u32 | index offset |
| 32 | u32 | frame-data offset |
| 36 | u32 | total file size |
| 40 | u32 | reserved |
| 44 | u32 | reserved |

El índice contiene `frame_count` offsets u32 absolutos.

## Frame

```text
u32 payload_size
u8  type          0=delta, 1=key
u8  reserved
u16 record_count
record[record_count] {
    u16 tile_index
    u16 rgb565
}
```

Un keyframe contiene exactamente todos los tiles. Un delta sólo contiene los
que cambiaron. Los tiles se numeran de izquierda a derecha y de arriba abajo.
