import 'dart:async';
import 'dart:typed_data';
import 'package:usb_serial/usb_serial.dart';
import 'esp_protocol.dart';

class FlashService {
  UsbPort? _port;
  StreamSubscription? _sub;
  final _rxBuffer = <int>[];
  final _rxCtrl = StreamController<Uint8List>.broadcast();

  Stream<Uint8List> get rxStream => _rxCtrl.stream;

  Future<List<UsbDevice>> getDevices() async {
    return await UsbSerial.listDevices();
  }

  Future<bool> connect(UsbDevice device, int baud) async {
    _port = await device.create();
    if (_port == null) return false;
    if (!await _port!.open()) return false;
    await _port!.setDTR(false);
    await _port!.setRTS(false);
    await _port!.setPortParameters(
      baud,
      UsbPort.DATABITS_8,
      UsbPort.STOPBITS_1,
      UsbPort.PARITY_NONE,
    );
    _sub = _port!.inputStream!.listen((data) {
      _rxBuffer.addAll(data);
      _rxCtrl.add(data);
    });
    return true;
  }

  Future<void> disconnect() async {
    await _sub?.cancel();
    await _port?.close();
    _port = null;
    _rxBuffer.clear();
  }

  Future<void> write(Uint8List data) async {
    await _port?.write(data);
  }

  Future<void> setDTR(bool v) async => await _port?.setDTR(v);
  Future<void> setRTS(bool v) async => await _port?.setRTS(v);

  // Enter bootloader via DTR/RTS signals
  Future<void> enterBootloaderSignals() async {
    try {
      await setDTR(false); await setRTS(true);
      await Future.delayed(const Duration(milliseconds: 100));
      await setDTR(true); await setRTS(false);
      await Future.delayed(const Duration(milliseconds: 50));
      await setDTR(false); await setRTS(false);
    } catch (_) {
      // Some adapters don't support DTR/RTS — manual boot mode needed
    }
    await Future.delayed(const Duration(milliseconds: 500));
  }

  // Read response with timeout — accumulates SLIP frame
  Future<Uint8List?> readResponse({int timeoutMs = 500}) async {
    final completer = Completer<Uint8List?>();
    final rawBytes = <int>[];
    int frameStart = -1;
    Timer? timer;
    StreamSubscription? sub;

    timer = Timer(Duration(milliseconds: timeoutMs), () {
      sub?.cancel();
      if (!completer.isCompleted) completer.complete(null);
    });

    sub = _rxCtrl.stream.listen((data) {
      for (final b in data) {
        rawBytes.add(b);
        final idx = rawBytes.length - 1;
        if (b == 0xC0) {
          if (frameStart == -1) {
            frameStart = idx;
          } else if (idx > frameStart) {
            // Complete frame received
            timer?.cancel();
            sub?.cancel();
            if (!completer.isCompleted) {
              completer.complete(Uint8List.fromList(rawBytes));
            }
            return;
          }
        }
      }
    });

    return completer.future;
  }

  // SYNC with retries
  Future<bool> sync({int retries = 7}) async {
    final syncPkt = buildSync();
    for (int i = 0; i < retries; i++) {
      _rxBuffer.clear();
      await write(syncPkt);
      await Future.delayed(const Duration(milliseconds: 50));
      final resp = await readResponse(timeoutMs: 300);
      if (resp != null && parseResponse(resp) == 0) return true;
      await Future.delayed(const Duration(milliseconds: 50));
    }
    return false;
  }

  // Flash a single firmware file
  Future<bool> flashFile({
    required Uint8List data,
    required int address,
    required String name,
    required void Function(double progress, String status) onProgress,
  }) async {
    // FLASH_BEGIN
    await write(buildFlashBegin(data.length, address));
    final beginResp = await readResponse(timeoutMs: 2000);
    if (beginResp == null || parseResponse(beginResp) != 0) {
      onProgress(0, '$name: FLASH_BEGIN failed');
      return false;
    }

    int offset = 0, seq = 0;
    while (offset < data.length) {
      final end = (offset + blockSize).clamp(0, data.length);
      final chunk = data.sublist(offset, end);
      await write(buildFlashData(chunk, seq++));

      // Validate every block
      final resp = await readResponse(timeoutMs: 1000);
      if (resp != null && parseResponse(resp) != 0) {
        onProgress(offset / data.length, '$name: block $seq error');
      }

      offset = end;
      onProgress(offset / data.length, 'Flashing $name... ${(offset / data.length * 100).toStringAsFixed(0)}%');
    }
    return true;
  }

  // Hard reset
  Future<void> hardReset() async {
    try {
      await setDTR(false); await setRTS(true);
      await Future.delayed(const Duration(milliseconds: 100));
      await setDTR(false); await setRTS(false);
    } catch (_) {}
  }
}
