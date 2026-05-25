import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:usb_serial/usb_serial.dart';
import 'package:file_picker/file_picker.dart';
import '../services/flash_service.dart';
import '../services/download_service.dart';
import '../services/esp_protocol.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final FlashService _flash = FlashService();
  final DownloadService _dl = DownloadService();

  List<UsbDevice> _devices = [];
  UsbDevice? _selected;
  bool _connected = false;
  bool _flashing = false;
  bool _downloading = false;

  int _baudRate = 115200;
  final List<int> _bauds = [921600, 460800, 115200];

  final Map<String, Uint8List?> _bufs = {
    'bootloader': null, 'partitions': null, 'firmware': null, 'littlefs': null,
  };

  final List<String> _logs = [];
  double _flashProgress = 0;
  double _dlProgress = 0;
  String _statusMsg = 'Ready';
  String _currentFile = '';

  void _log(String msg, {String type = 'info'}) {
    setState(() => _logs.insert(0, '[${DateTime.now().toLocal().toString().substring(11, 19)}] $msg'));
    if (_logs.length > 100) _logs.removeLast();
  }

  Future<void> _scanDevices() async {
    final devs = await _flash.getDevices();
    setState(() => _devices = devs);
    if (devs.isEmpty) {
      _log('No USB devices found — check OTG connection', type: 'warn');
    } else {
      _log('Found ${devs.length} device(s)');
    }
  }

  Future<void> _connect() async {
    if (_selected == null) { _log('Select a device first'); return; }
    final ok = await _flash.connect(_selected!, _baudRate);
    if (ok) {
      setState(() => _connected = true);
      _log('Connected @ $_baudRate baud ✅');
    } else {
      _log('Connection failed ❌');
    }
  }

  Future<void> _disconnect() async {
    await _flash.disconnect();
    setState(() { _connected = false; _selected = null; });
    _log('Disconnected');
  }

  Future<void> _downloadAll() async {
    setState(() { _downloading = true; _dlProgress = 0; });
    _log('Downloading firmware files...');

    for (int i = 0; i < firmwareFiles.length; i++) {
      final f = firmwareFiles[i];
      final fname = f['name'] as String;
      final key = f['key'] as String;
      setState(() => _currentFile = fname);
      _log('Downloading $fname...');

      final bytes = await _dl.downloadFile(fname, onProgress: (p) {
        setState(() => _dlProgress = (i + p) / firmwareFiles.length);
      });

      if (bytes == null) {
        _log('Failed: $fname ❌');
        setState(() => _downloading = false);
        return;
      }
      _bufs[key] = bytes;
      _log('$fname ready — ${(bytes.length / 1024).toStringAsFixed(1)} KB ✅');
      setState(() => _dlProgress = (i + 1) / firmwareFiles.length);
    }

    setState(() { _downloading = false; _dlProgress = 1.0; });
    _log('All firmware files ready ✅');
  }

  Future<void> _pickLocalFiles() async {
    final result = await FilePicker.platform.pickFiles(
      allowMultiple: true,
      type: FileType.custom,
      allowedExtensions: ['bin'],
    );
    if (result == null) return;

    for (final file in result.files) {
      final name = file.name;
      final bytes = file.bytes;
      if (bytes == null) continue;

      final match = firmwareFiles.firstWhere(
        (f) => f['name'] == name, orElse: () => {},
      );
      if (match.isEmpty) { _log('Unknown file: $name', type: 'warn'); continue; }
      _bufs[match['key'] as String] = bytes;
      _log('Loaded $name — ${(bytes.length / 1024).toStringAsFixed(1)} KB ✅');
    }
    setState(() {});
  }

  bool get _filesReady => _bufs.values.every((b) => b != null);

  Future<void> _startFlash() async {
    if (!_connected) { _log('Connect device first ❌'); return; }
    if (!_filesReady) { _log('Download firmware files first ❌'); return; }

    final confirm = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF0F0F18),
        title: const Text('Flash Firmware?', style: TextStyle(color: Color(0xFF00FFB4))),
        content: const Text('4 firmware files ESP32 pe flash honge.\nDevice BOOT mode mein hona chahiye.',
            style: TextStyle(color: Colors.white70)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          ElevatedButton(
            style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF00FFB4)),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Flash', style: TextStyle(color: Colors.black)),
          ),
        ],
      ),
    );
    if (confirm != true) return;

    setState(() { _flashing = true; _flashProgress = 0; });
    _log('Entering bootloader...');

    await _flash.enterBootloaderSignals();

    final synced = await _flash.sync();
    if (!synced) {
      _log('SYNC failed — hold BOOT button, press RESET, then try again ❌');
      setState(() => _flashing = false);
      return;
    }
    _log('Bootloader SYNC ✅');

    double totalBytes = firmwareFiles.fold(0, (s, f) => s + (_bufs[f['key']]?.length ?? 0));
    double doneBytes = 0;

    for (final f in firmwareFiles) {
      final key = f['key'] as String;
      final addr = f['addr'] as int;
      final name = f['name'] as String;
      final data = _bufs[key]!;

      if (data.isEmpty) { _log('$name is empty — skipping ⚠️'); continue; }

      _log('Flashing $name @ 0x${addr.toRadixString(16)}...');

      final ok = await _flash.flashFile(
        data: data, address: addr, name: name,
        onProgress: (p, status) {
          setState(() {
            _flashProgress = (doneBytes + data.length * p) / totalBytes;
            _statusMsg = status;
          });
        },
      );

      if (!ok) {
        _log('$name flash failed ❌');
        setState(() => _flashing = false);
        return;
      }
      doneBytes += data.length;
      _log('$name done ✅');
    }

    await _flash.hardReset();
    setState(() { _flashing = false; _flashProgress = 1.0; _statusMsg = 'Flash complete!'; });
    _log('🎉 Flash complete! Device boot ho raha hai...');
    _log('Browser mein kholo: http://ir-remote.local');

    if (mounted) {
      showDialog(
        context: context,
        builder: (ctx) => AlertDialog(
          backgroundColor: const Color(0xFF0F0F18),
          title: const Text('🎉 Flash Complete!', style: TextStyle(color: Color(0xFF00FFB4))),
          content: const Text('Device boot ho raha hai.\nBrowser mein kholo:\nhttp://ir-remote.local',
              style: TextStyle(color: Colors.white70)),
          actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('OK'))],
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: const Color(0xFF0A0A12),
        title: const Text('IR Remote Flasher', style: TextStyle(color: Color(0xFF00FFB4), fontWeight: FontWeight.bold)),
        actions: [
          IconButton(icon: const Icon(Icons.refresh, color: Color(0xFF00FFB4)), onPressed: _scanDevices, tooltip: 'Scan USB'),
        ],
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(children: [

          // ── DEVICE CARD ──
          _card('🔌 USB Device', [
            Row(children: [
              Expanded(
                child: DropdownButton<UsbDevice>(
                  dropdownColor: const Color(0xFF0F0F18),
                  value: _selected,
                  hint: const Text('Select USB device', style: TextStyle(color: Colors.white54)),
                  isExpanded: true,
                  items: _devices.map((d) => DropdownMenuItem(
                    value: d,
                    child: Text('${d.productName ?? d.manufacturerName ?? 'Unknown'} (${d.vid?.toRadixString(16)})',
                        style: const TextStyle(color: Colors.white)),
                  )).toList(),
                  onChanged: _connected ? null : (v) => setState(() => _selected = v),
                ),
              ),
              const SizedBox(width: 8),
              DropdownButton<int>(
                dropdownColor: const Color(0xFF0F0F18),
                value: _baudRate,
                items: _bauds.map((b) => DropdownMenuItem(
                  value: b,
                  child: Text('$b', style: const TextStyle(color: Colors.white, fontSize: 12)),
                )).toList(),
                onChanged: _connected ? null : (v) => setState(() => _baudRate = v!),
              ),
            ]),
            const SizedBox(height: 8),
            Row(children: [
              ElevatedButton.icon(
                style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF00FFB4)),
                onPressed: _devices.isEmpty ? _scanDevices : (_connected ? null : _connect),
                icon: Icon(_devices.isEmpty ? Icons.search : Icons.usb, color: Colors.black),
                label: Text(_devices.isEmpty ? 'Scan' : 'Connect', style: const TextStyle(color: Colors.black)),
              ),
              const SizedBox(width: 8),
              if (_connected)
                OutlinedButton.icon(
                  style: OutlinedButton.styleFrom(side: const BorderSide(color: Colors.redAccent)),
                  onPressed: _flashing ? null : _disconnect,
                  icon: const Icon(Icons.link_off, color: Colors.redAccent),
                  label: const Text('Disconnect', style: TextStyle(color: Colors.redAccent)),
                ),
            ]),
            if (_connected)
              const Padding(
                padding: EdgeInsets.only(top: 8),
                child: Row(children: [
                  Icon(Icons.circle, color: Color(0xFF00FFB4), size: 10),
                  SizedBox(width: 6),
                  Text('Connected', style: TextStyle(color: Color(0xFF00FFB4), fontSize: 12)),
                ]),
              ),
          ]),

          const SizedBox(height: 12),

          // ── FIRMWARE CARD ──
          _card('📦 Firmware Files', [
            ..._bufs.entries.map((e) => Padding(
              padding: const EdgeInsets.symmetric(vertical: 3),
              child: Row(children: [
                Icon(e.value != null ? Icons.check_circle : Icons.radio_button_unchecked,
                    color: e.value != null ? const Color(0xFF00FFB4) : Colors.white38, size: 16),
                const SizedBox(width: 8),
                Text('${e.key}.bin', style: const TextStyle(color: Colors.white70, fontSize: 12, fontFamily: 'monospace')),
                if (e.value != null)
                  Text('  ${(e.value!.length / 1024).toStringAsFixed(1)} KB',
                      style: const TextStyle(color: Colors.white38, fontSize: 11)),
              ]),
            )),
            const SizedBox(height: 10),
            if (_downloading) ...[
              Text('Downloading $_currentFile...', style: const TextStyle(color: Colors.white54, fontSize: 12)),
              const SizedBox(height: 6),
              LinearProgressIndicator(value: _dlProgress, color: const Color(0xFF00FFB4), backgroundColor: Colors.white12),
              const SizedBox(height: 8),
            ],
            Row(children: [
              ElevatedButton.icon(
                style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF38BDF8)),
                onPressed: _downloading || _flashing ? null : _downloadAll,
                icon: const Icon(Icons.download, color: Colors.black),
                label: const Text('Auto Download', style: TextStyle(color: Colors.black)),
              ),
              const SizedBox(width: 8),
              OutlinedButton.icon(
                style: OutlinedButton.styleFrom(side: const BorderSide(color: Colors.white38)),
                onPressed: _flashing ? null : _pickLocalFiles,
                icon: const Icon(Icons.folder_open, color: Colors.white54),
                label: const Text('Local Files', style: TextStyle(color: Colors.white54)),
              ),
            ]),
          ]),

          const SizedBox(height: 12),

          // ── FLASH CARD ──
          _card('⚡ Flash Firmware', [
            Container(
              padding: const EdgeInsets.all(10),
              decoration: BoxDecoration(
                color: Colors.amber.withOpacity(0.08),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.amber.withOpacity(0.3)),
              ),
              child: const Text(
                'BOOT mode: BOOT button hold → RESET press → BOOT release',
                style: TextStyle(color: Colors.amber, fontSize: 12, fontFamily: 'monospace'),
              ),
            ),
            const SizedBox(height: 10),
            if (_flashing) ...[
              Text(_statusMsg, style: const TextStyle(color: Colors.white54, fontSize: 12)),
              const SizedBox(height: 6),
              LinearProgressIndicator(value: _flashProgress, color: const Color(0xFF00FFB4), backgroundColor: Colors.white12),
              Text('${(_flashProgress * 100).toStringAsFixed(0)}%',
                  style: const TextStyle(color: Color(0xFF00FFB4), fontSize: 12)),
              const SizedBox(height: 8),
            ],
            SizedBox(
              width: double.infinity,
              child: ElevatedButton.icon(
                style: ElevatedButton.styleFrom(
                  backgroundColor: _connected && _filesReady && !_flashing
                      ? const Color(0xFF00FFB4) : Colors.white12,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                ),
                onPressed: _connected && _filesReady && !_flashing ? _startFlash : null,
                icon: Icon(Icons.flash_on, color: _connected && _filesReady ? Colors.black : Colors.white38),
                label: Text(
                  _flashing ? 'Flashing...' : 'Flash Now',
                  style: TextStyle(
                    color: _connected && _filesReady && !_flashing ? Colors.black : Colors.white38,
                    fontSize: 16, fontWeight: FontWeight.bold,
                  ),
                ),
              ),
            ),
          ]),

          const SizedBox(height: 12),

          // ── LOG CARD ──
          _card('📋 Log', [
            Container(
              height: 180,
              decoration: BoxDecoration(
                color: Colors.black45,
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.white12),
              ),
              child: ListView.builder(
                reverse: false,
                padding: const EdgeInsets.all(8),
                itemCount: _logs.length,
                itemBuilder: (ctx, i) => Text(
                  _logs[i],
                  style: TextStyle(
                    fontSize: 11,
                    fontFamily: 'monospace',
                    color: _logs[i].contains('❌') ? Colors.redAccent
                        : _logs[i].contains('✅') ? const Color(0xFF00FFB4)
                        : _logs[i].contains('⚠️') ? Colors.amber
                        : Colors.white54,
                  ),
                ),
              ),
            ),
            TextButton.icon(
              onPressed: () => setState(() => _logs.clear()),
              icon: const Icon(Icons.delete_outline, size: 14, color: Colors.white38),
              label: const Text('Clear', style: TextStyle(color: Colors.white38, fontSize: 12)),
            ),
          ]),

        ]),
      ),
    );
  }

  Widget _card(String title, List<Widget> children) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF0F0F18),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(title, style: const TextStyle(
          color: Color(0xFF00FFB4), fontSize: 12,
          fontFamily: 'monospace', letterSpacing: 1.5,
          fontWeight: FontWeight.bold,
        )),
        const SizedBox(height: 12),
        ...children,
      ]),
    );
  }
}
