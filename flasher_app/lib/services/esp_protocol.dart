import 'dart:typed_data';

const int blockSize = 0x200;
const int backupSize = 0x100000;

// SLIP encode
Uint8List slipEncode(Uint8List data) {
  final out = <int>[0xC0];
  for (final b in data) {
    if (b == 0xC0) {
      out.addAll([0xDB, 0xDC]);
    } else if (b == 0xDB) {
      out.addAll([0xDB, 0xDD]);
    } else {
      out.add(b);
    }
  }
  out.add(0xC0);
  return Uint8List.fromList(out);
}

// SLIP decode
Uint8List slipDecode(Uint8List bytes) {
  final out = <int>[];
  int i = 0;
  while (i < bytes.length) {
    final b = bytes[i++];
    if (b == 0xC0) continue;
    if (b == 0xDB && i < bytes.length) {
      final nb = bytes[i++];
      if (nb == 0xDC) {
        out.add(0xC0);
      } else if (nb == 0xDD) {
        out.add(0xDB);
      } else {
        out.add(b);
        out.add(nb);
      }
    } else {
      out.add(b);
    }
  }
  return Uint8List.fromList(out);
}

// Build ESP32 ROM command packet
Uint8List buildCmd(int op, Uint8List data, {int chk = 0}) {
  final pkt = ByteData(8 + data.length);
  pkt.setUint8(0, 0x00);
  pkt.setUint8(1, op);
  pkt.setUint16(2, data.length, Endian.little);
  pkt.setUint32(4, chk, Endian.little);
  final result = pkt.buffer.asUint8List();
  result.setRange(8, 8 + data.length, data);
  return result;
}

// Build SYNC packet
Uint8List buildSync() {
  final sd = Uint8List(36);
  sd[0] = 0x07; sd[1] = 0x07; sd[2] = 0x12; sd[3] = 0x20;
  for (int i = 4; i < 36; i++) sd[i] = 0x55;
  return slipEncode(buildCmd(0x08, sd));
}

// Build FLASH_BEGIN packet
Uint8List buildFlashBegin(int size, int addr) {
  final bd = ByteData(16);
  bd.setUint32(0, size, Endian.little);
  bd.setUint32(4, (size / blockSize).ceil(), Endian.little);
  bd.setUint32(8, blockSize, Endian.little);
  bd.setUint32(12, addr, Endian.little);
  return slipEncode(buildCmd(0x02, bd.buffer.asUint8List()));
}

// Build FLASH_DATA packet with XOR checksum
Uint8List buildFlashData(Uint8List chunk, int seq) {
  final block = ByteData(16 + chunk.length);
  block.setUint32(0, chunk.length, Endian.little);
  block.setUint32(4, seq, Endian.little);
  block.setUint32(8, 0, Endian.little);
  block.setUint32(12, 0, Endian.little);
  final blockBytes = block.buffer.asUint8List();
  blockBytes.setRange(16, 16 + chunk.length, chunk);
  // XOR checksum over full block
  int chk = 0xEF;
  for (final b in blockBytes) chk ^= b;
  return slipEncode(buildCmd(0x03, blockBytes, chk: chk));
}

// Build FLASH_ERASE packet
Uint8List buildFlashErase() {
  return slipEncode(buildCmd(0x04, Uint8List(0)));
}

// Parse ESP32 response — returns status byte
// Returns -1 on invalid, 0 on success, >0 on error
int parseResponse(Uint8List raw) {
  final decoded = slipDecode(raw);
  if (decoded.length < 10) return -1;
  if (decoded[0] != 0x01) return -1;
  return decoded[8];
}
