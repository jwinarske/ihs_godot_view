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

import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'native.dart';

/// Godot JoyAxis numbering.
abstract final class JoyAxis {
  static const leftX = 0;
  static const leftY = 1;
  static const rightX = 2;
  static const rightY = 3;
  static const triggerLeft = 4;
  static const triggerRight = 5;
}

/// Godot JoyButton numbering (SDL layout).
abstract final class JoyButton {
  static const a = 0;
  static const b = 1;
  static const x = 2;
  static const y = 3;
  static const back = 4;
  static const guide = 5;
  static const start = 6;
}

/// A trigger or button on the virtual pad.
class VirtualButton {
  const VirtualButton.button(this.label, this.index) : isAxis = false;
  const VirtualButton.trigger(this.label, this.index) : isAxis = true;

  final String label;
  final int index;

  /// Drives a trigger axis (0..1) instead of a button.
  final bool isAxis;
}

/// An on-screen gamepad drawn by Flutter over a [GodotView] and fed to Godot
/// as a real joypad (Input.joy_connection_changed, joy_axis, joy_button).
///
/// Games that are played with a pad -- the Godot third-person shooter demo
/// maps move/look/aim/shoot to sticks and triggers -- become playable on a
/// touch-only head unit without changing the game.
class VirtualGamepad extends StatefulWidget {
  const VirtualGamepad({
    super.key,
    this.device = 7,
    this.leftStick = true,
    this.rightStick = true,
    this.buttons = const <VirtualButton>[
      VirtualButton.button('A', JoyButton.a),
      VirtualButton.trigger('AIM', JoyAxis.triggerLeft),
      VirtualButton.trigger('FIRE', JoyAxis.triggerRight),
    ],
    this.opacity = 0.45,
  });

  /// Godot joypad device id. Pads Godot reads itself (SDL) number from 0, so
  /// keep this clear of them.
  final int device;
  final bool leftStick;
  final bool rightStick;
  final List<VirtualButton> buttons;
  final double opacity;

  @override
  State<VirtualGamepad> createState() => _VirtualGamepadState();
}

class _VirtualGamepadState extends State<VirtualGamepad> {
  GodotNative? _native;

  @override
  void initState() {
    super.initState();
    try {
      _native = GodotNative.instance
        ..joyConnection(widget.device, connected: true, name: 'Flutter pad');
    } on Object {
      _native = null;
    }
  }

  @override
  void dispose() {
    _native?.joyConnection(widget.device, connected: false);
    super.dispose();
  }

  void _axis(int axis, double v) => _native?.joyAxis(widget.device, axis, v);
  void _button(int b, bool down) =>
      _native?.joyButton(widget.device, b, down ? 1 : 0);

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: widget.opacity,
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            if (widget.leftStick)
              _Stick(onChanged: (o) {
                _axis(JoyAxis.leftX, o.dx);
                _axis(JoyAxis.leftY, o.dy);
              }),
            const Spacer(),
            Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                for (final b in widget.buttons)
                  Padding(
                    padding: const EdgeInsets.all(6),
                    child: _PadButton(
                      label: b.label,
                      onChanged: (down) => b.isAxis
                          ? _axis(b.index, down ? 1.0 : 0.0)
                          : _button(b.index, down),
                    ),
                  ),
              ],
            ),
            const SizedBox(width: 16),
            if (widget.rightStick)
              _Stick(onChanged: (o) {
                _axis(JoyAxis.rightX, o.dx);
                _axis(JoyAxis.rightY, o.dy);
              }),
          ],
        ),
      ),
    );
  }
}

class _Stick extends StatefulWidget {
  const _Stick({required this.onChanged});

  final ValueChanged<Offset> onChanged;

  @override
  State<_Stick> createState() => _StickState();
}

class _StickState extends State<_Stick> {
  static const double _radius = 64;
  Offset _knob = Offset.zero;

  void _update(Offset local) {
    var v = local - const Offset(_radius, _radius);
    if (v.distance > _radius) {
      v = Offset.fromDirection(v.direction, _radius);
    }
    setState(() => _knob = v);
    widget.onChanged(Offset(v.dx / _radius, v.dy / _radius));
  }

  void _release() {
    setState(() => _knob = Offset.zero);
    widget.onChanged(Offset.zero);
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onPanStart: (d) => _update(d.localPosition),
      onPanUpdate: (d) => _update(d.localPosition),
      onPanEnd: (_) => _release(),
      onPanCancel: _release,
      child: SizedBox.square(
        dimension: _radius * 2,
        child: CustomPaint(painter: _StickPainter(_knob, _radius)),
      ),
    );
  }
}

class _StickPainter extends CustomPainter {
  _StickPainter(this.knob, this.radius);

  final Offset knob;
  final double radius;

  @override
  void paint(Canvas canvas, Size size) {
    final c = Offset(radius, radius);
    canvas.drawCircle(c, radius, Paint()..color = Colors.white24);
    canvas.drawCircle(
        c,
        radius,
        Paint()
          ..color = Colors.white70
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2);
    canvas.drawCircle(c + knob, radius * 0.4, Paint()..color = Colors.white70);
  }

  @override
  bool shouldRepaint(_StickPainter old) => old.knob != knob;
}

class _PadButton extends StatefulWidget {
  const _PadButton({required this.label, required this.onChanged});

  final String label;
  final ValueChanged<bool> onChanged;

  @override
  State<_PadButton> createState() => _PadButtonState();
}

class _PadButtonState extends State<_PadButton> {
  bool _down = false;

  void _set(bool down) {
    if (_down == down) {
      return;
    }
    setState(() => _down = down);
    widget.onChanged(down);
  }

  @override
  Widget build(BuildContext context) {
    final size = math.max(56.0, widget.label.length * 14.0);
    return Listener(
      onPointerDown: (_) => _set(true),
      onPointerUp: (_) => _set(false),
      onPointerCancel: (_) => _set(false),
      child: Container(
        width: size,
        height: 56,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: _down ? Colors.white70 : Colors.white24,
          borderRadius: BorderRadius.circular(28),
          border: Border.all(color: Colors.white70, width: 2),
        ),
        child: Text(widget.label,
            style: TextStyle(
                // Bundled with this package: the target may have no fonts.
                fontFamily: 'IhsGodotViewLabel',
                package: 'ihs_godot_view',
                color: _down ? Colors.black : Colors.white,
                fontWeight: FontWeight.bold)),
      ),
    );
  }
}
