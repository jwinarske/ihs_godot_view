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

import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'godot_controller.dart';
import 'native.dart';

/// The platform-view type libihs_godot_view registers.
const String kGodotViewType = 'ihs_godot_view';

/// How frames reach the compositor. [auto] picks the best path the active
/// ivi-homescreen backend and Godot's renderer share.
enum GodotSurface { auto, dmabuf, plane, vkImage, eglImage, shm }

extension on GodotSurface {
  String get wire => switch (this) {
        GodotSurface.auto => 'auto',
        GodotSurface.dmabuf => 'dmabuf',
        GodotSurface.plane => 'plane',
        GodotSurface.vkImage => 'vk_image',
        GodotSurface.eglImage => 'egl_image',
        GodotSurface.shm => 'shm',
      };
}

/// A Godot game, rendered by LibGodot and composited by ivi-homescreen.
///
/// Give it either an exported pack ([mainPack], the shipping form) or a
/// project directory ([projectPath], development builds of libgodot only).
/// Relative paths resolve against the bundle's flutter_assets.
///
/// Input: pointer events (touch, mouse incl. buttons and wheel, stylus) and
/// keys while focused are forwarded to the game. Gamepads attached to the
/// device are read by Godot itself; [VirtualGamepad] adds an on-screen one.
class GodotView extends StatefulWidget {
  const GodotView({
    super.key,
    this.mainPack,
    this.projectPath,
    this.args = const <String>[],
    this.surface = GodotSurface.auto,
    this.renderingDriver,
    this.renderingMethod,
    this.libgodot,
    this.maxFps,
    this.controller,
    this.autofocus = true,
  }) : assert(mainPack != null || projectPath != null,
            'GodotView needs a mainPack or a projectPath');

  final String? mainPack;
  final String? projectPath;
  final List<String> args;
  final GodotSurface surface;

  /// vulkan | opengl3 | opengl3_es; null: the project's choice.
  final String? renderingDriver;

  /// forward_plus | mobile | gl_compatibility; null: the project's choice.
  final String? renderingMethod;

  /// Path to libgodot.so; null: beside libihs_godot_view.so.
  final String? libgodot;
  final int? maxFps;
  final GodotController? controller;
  final bool autofocus;

  @override
  State<GodotView> createState() => _GodotViewState();
}

class _GodotViewState extends State<GodotView> {
  static bool _registered = false;
  final FocusNode _focus = FocusNode(debugLabel: 'GodotView');
  _GodotPlatformViewController? _view;
  GodotNative? _native;
  Object? _loadError;

  @override
  void initState() {
    super.initState();
    try {
      _native = GodotNative.instance;
      if (!_registered) {
        // A hot restart keeps the native side registered; registering again
        // just replaces the factory.
        _native!.register();
        _registered = true;
      }
    } on Object catch (e) {
      _loadError = e;
    }
  }

  @override
  void dispose() {
    _focus.dispose();
    super.dispose();
  }

  Map<String, Object?> _params(double dpr) => <String, Object?>{
        if (widget.mainPack != null) 'main_pack': widget.mainPack,
        if (widget.projectPath != null) 'path': widget.projectPath,
        if (widget.args.isNotEmpty) 'args': widget.args,
        'surface': widget.surface.wire,
        if (widget.renderingDriver != null)
          'rendering_driver': widget.renderingDriver,
        if (widget.renderingMethod != null)
          'rendering_method': widget.renderingMethod,
        if (widget.libgodot != null) 'libgodot': widget.libgodot,
        if (widget.maxFps != null) 'max_fps': widget.maxFps,
        'device_pixel_ratio': dpr,
      };

  // --- input ---------------------------------------------------------------

  static int _kind(PointerDeviceKind k) => switch (k) {
        PointerDeviceKind.touch => 0,
        PointerDeviceKind.mouse => 1,
        PointerDeviceKind.stylus || PointerDeviceKind.invertedStylus => 2,
        PointerDeviceKind.trackpad => 3,
        PointerDeviceKind.unknown => 0,
      };

  void _pointer(PointerEvent e, int phase,
      {double scrollDx = 0, double scrollDy = 0}) {
    final native = _native;
    final view = _view;
    if (native == null || view == null) {
      return;
    }
    native.pointer(
        view.viewId,
        _kind(e.kind),
        phase,
        e.pointer,
        e.localPosition.dx,
        e.localPosition.dy,
        e.localDelta.dx,
        e.localDelta.dy,
        e.buttons,
        e.pressure.isFinite ? e.pressure : 1.0,
        scrollDx,
        scrollDy);
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    final native = _native;
    if (native == null) {
      return KeyEventResult.ignored;
    }
    final keyboard = HardwareKeyboard.instance;
    final mods = (keyboard.isShiftPressed ? 1 : 0) |
        (keyboard.isControlPressed ? 2 : 0) |
        (keyboard.isAltPressed ? 4 : 0) |
        (keyboard.isMetaPressed ? 8 : 0);
    final ch = event.character;
    final unicode = (ch != null && ch.isNotEmpty) ? ch.runes.first : 0;
    native.key(event.physicalKey.usbHidUsage, unicode,
        event is KeyUpEvent ? 0 : 1, event is KeyRepeatEvent ? 1 : 0, mods);
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    if (_loadError != null) {
      return ErrorWidget.withDetails(message: _loadError.toString());
    }
    final dpr = MediaQuery.devicePixelRatioOf(context);
    return Focus(
      focusNode: _focus,
      autofocus: widget.autofocus,
      onKeyEvent: _onKey,
      child: Listener(
        onPointerDown: (e) {
          _focus.requestFocus();
          _pointer(e, 0);
        },
        onPointerMove: (e) => _pointer(e, 1),
        onPointerHover: (e) => _pointer(e, 1),
        onPointerUp: (e) => _pointer(e, 2),
        onPointerCancel: (e) => _pointer(e, 3),
        onPointerSignal: (e) {
          if (e is PointerScrollEvent) {
            _pointer(e, 4,
                scrollDx: e.scrollDelta.dx, scrollDy: e.scrollDelta.dy);
          }
        },
        child: LayoutBuilder(builder: (context, constraints) {
          final size = Size(constraints.maxWidth * dpr,
              constraints.maxHeight * dpr);
          _view?.resize(size);
          return PlatformViewLink(
            viewType: kGodotViewType,
            surfaceFactory: (context, controller) => PlatformViewSurface(
              controller: controller,
              // Input goes through the Listener above and FFI, which carries
              // buttons, wheel and every contact; the platform-view touch
              // channel carries one contact's coordinates.
              hitTestBehavior: PlatformViewHitTestBehavior.transparent,
              gestureRecognizers: const <Factory<OneSequenceGestureRecognizer>>{},
            ),
            onCreatePlatformView: (params) {
              final view = _GodotPlatformViewController(
                id: params.id,
                size: size,
                params: _params(dpr),
              );
              _view = view;
              widget.controller?.attach(params.id);
              unawaited(view.create().then((_) {
                params.onPlatformViewCreated(params.id);
              }));
              return view;
            },
          );
        }),
      ),
    );
  }

  @override
  void deactivate() {
    if (_view != null) {
      widget.controller?.detach(_view!.viewId);
    }
    super.deactivate();
  }
}

// Creates the view over the flutter/platform_views channel the way
// ivi-homescreen's registry expects: a create map with the view type, the
// size in physical pixels, and StandardMessageCodec-encoded params.
class _GodotPlatformViewController extends PlatformViewController {
  _GodotPlatformViewController({
    required this.id,
    required Size size,
    required this.params,
  }) : _size = size;

  final int id;
  final Map<String, Object?> params;
  Size _size;
  bool _created = false;
  bool _disposed = false;
  Future<void>? _creation;

  @override
  int get viewId => id;

  @override
  bool get awaitingCreation => !_created;

  @override
  Future<void> create({Size? size, Offset? position}) =>
      _creation ??= _createOnce();

  Future<void> _createOnce() async {
    final encoded = const StandardMessageCodec().encodeMessage(params)!;
    await SystemChannels.platform_views.invokeMethod<void>('create', {
      'id': id,
      'viewType': kGodotViewType,
      'direction': 0,
      'width': _size.width,
      'height': _size.height,
      'params': encoded.buffer.asUint8List(
          encoded.offsetInBytes, encoded.lengthInBytes),
    });
    _created = true;
    if (_disposed) {
      // Disposed while the create was in flight.
      await _sendDispose();
    }
  }

  void resize(Size size) {
    if (size == _size || size.isEmpty) {
      return;
    }
    _size = size;
    if (!_created || _disposed) {
      return;
    }
    unawaited(SystemChannels.platform_views.invokeMethod<void>('resize', {
      'id': id,
      'width': size.width,
      'height': size.height,
    }));
  }

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {}

  @override
  Future<void> clearFocus() async {}

  Future<void> _sendDispose() => SystemChannels.platform_views
      .invokeMethod<void>('dispose', <String, Object>{'id': id});

  @override
  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    if (_created) {
      await _sendDispose();
    }
  }
}
