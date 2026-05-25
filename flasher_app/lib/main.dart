import 'package:flutter/material.dart';
import 'screens/home_screen.dart';

void main() {
  runApp(const IRFlasherApp());
}

class IRFlasherApp extends StatelessWidget {
  const IRFlasherApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'IR Remote Flasher',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF00FFB4),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
        scaffoldBackgroundColor: const Color(0xFF050508),
        cardColor: const Color(0xFF0F0F18),
        fontFamily: 'monospace',
      ),
      home: const HomeScreen(),
    );
  }
}
