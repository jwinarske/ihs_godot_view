// Copyright 2026 Joel Winarske
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The Godot third-person shooter demo (a fork of godotengine/tps-demo) in a
// Flutter app on ivi-homescreen. Flutter draws a status chip and, on touch
// screens, an on-screen gamepad over the game; the game itself is a platform
// view fed by LibGodot.
//
// The exported pack is looked up, in order:
//   --dart-define=GODOT_MAIN_PACK=<path>   (relative: under flutter_assets)
//   IHS_GODOT_MAIN_PACK in the environment  (wins over the define; native)
//   godot/tps-demo.pck under flutter_assets

import 'package:flutter/material.dart';
import 'package:ihs_godot_view/ihs_godot_view.dart';

const String kMainPack =
    String.fromEnvironment('GODOT_MAIN_PACK', defaultValue: 'godot/tps-demo.pck');
const String kSurface = String.fromEnvironment('GODOT_SURFACE', defaultValue: 'auto');

GodotSurface _surface(String s) => switch (s) {
      'dmabuf' => GodotSurface.dmabuf,
      'plane' => GodotSurface.plane,
      'vk_image' => GodotSurface.vkImage,
      'egl_image' => GodotSurface.eglImage,
      'shm' => GodotSurface.shm,
      _ => GodotSurface.auto,
    };

void main() {
  runApp(const TpsDemoApp());
}

class TpsDemoApp extends StatelessWidget {
  const TpsDemoApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const TpsDemoPage(),
    );
  }
}

class TpsDemoPage extends StatefulWidget {
  const TpsDemoPage({super.key});

  @override
  State<TpsDemoPage> createState() => _TpsDemoPageState();
}

class _TpsDemoPageState extends State<TpsDemoPage> {
  final GodotController _godot = GodotController();
  bool _showPad = true;

  @override
  void initState() {
    super.initState();
    _godot.addListener(() => setState(() {}));
    _godot.messages.listen((m) => debugPrint('[tps-demo] $m'));
  }

  @override
  void dispose() {
    _godot.dispose();
    super.dispose();
  }

  String get _status => switch (_godot.status) {
        GodotStatus.starting => 'starting Godot...',
        GodotStatus.running =>
          '${_godot.api ?? '?'} -> ${_godot.surfaceKind ?? '?'} on ${_godot.backend ?? '?'}',
        GodotStatus.quit => 'the game quit',
        GodotStatus.failed => 'failed: ${_godot.error ?? 'unknown'}',
      };

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        fit: StackFit.expand,
        children: [
          if (_godot.status != GodotStatus.quit)
            MouseRegion(
              cursor: _godot.mouseCaptured
                  ? SystemMouseCursors.none
                  : SystemMouseCursors.basic,
              child: GodotView(
                mainPack: kMainPack,
                surface: _surface(kSurface),
                controller: _godot,
              ),
            )
          else
            const Center(child: Text('Thanks for playing.')),
          Positioned(
            top: 12,
            left: 12,
            child: GestureDetector(
              onTap: () => setState(() => _showPad = !_showPad),
              child: Chip(
                avatar: Icon(_showPad ? Icons.gamepad : Icons.gamepad_outlined,
                    size: 18),
                label: Text(_status),
              ),
            ),
          ),
          if (_showPad && _godot.status == GodotStatus.running)
            const Align(
              alignment: Alignment.bottomCenter,
              child: VirtualGamepad(),
            ),
        ],
      ),
    );
  }
}
