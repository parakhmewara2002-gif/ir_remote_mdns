import 'dart:typed_data';
import 'package:http/http.dart' as http;

const String releaseBase = 'https://parakhmewara2002-gif.github.io/ir_remote_mdns/firmware/';
const String rawBase = 'https://raw.githubusercontent.com/parakhmewara2002-gif/ir_remote_mdns/master/firmware/';

const List<Map<String, dynamic>> firmwareFiles = [
  {'name': 'bootloader.bin',  'addr': 0x1000,   'key': 'bootloader'},
  {'name': 'partitions.bin',  'addr': 0x8000,   'key': 'partitions'},
  {'name': 'firmware.bin',    'addr': 0x10000,  'key': 'firmware'},
  {'name': 'littlefs.bin',    'addr': 0x310000, 'key': 'littlefs'},
];

class DownloadService {
  Future<Uint8List?> downloadFile(
    String filename, {
    void Function(double progress)? onProgress,
  }) async {
    final urls = [
      releaseBase + filename,
      rawBase + filename,
    ];

    for (final url in urls) {
      try {
        final client = http.Client();
        final request = http.Request('GET', Uri.parse(url));
        final response = await client.send(request);

        if (response.statusCode == 200) {
          final total = response.contentLength ?? 0;
          int received = 0;
          final bytes = <int>[];

          await for (final chunk in response.stream) {
            bytes.addAll(chunk);
            received += chunk.length;
            if (total > 0) onProgress?.call(received / total);
          }
          client.close();
          return Uint8List.fromList(bytes);
        }
        client.close();
      } catch (_) {
        continue;
      }
    }
    return null;
  }
}
