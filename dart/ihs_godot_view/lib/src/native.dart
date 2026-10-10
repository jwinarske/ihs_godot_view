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
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

/// Events from the native producer (IhsGodotViewEvent).
enum GodotEventType {
  /// The engine is up and the view has a surface path. [GodotViewEvent.text]
  /// is JSON: `{"kind": "dmabuf|plane|vk_image|egl_image|shm", "api": ...,
  /// "backend": ...}`.
  ready,

  /// `IhsHost.send_message(text)` from the game.
  message,

  /// The game changed `Input.mouse_mode`; the text is the mode number
  /// (2 = captured).
  mouseMode,

  /// The game quit. Godot cannot be started again in this process.
  quit,

  /// Something failed; the text says what.
  error,

  /// The surface path changed after a renegotiation (same payload as ready).
  surface,
}

class GodotViewEvent {
  const GodotViewEvent(this.viewId, this.type, this.text);

  final int viewId;
  final GodotEventType type;
  final String text;

  @override
  String toString() => 'GodotViewEvent($viewId, ${type.name}, $text)';
}

typedef _ListenerNative = Void Function(Int32, Int32, Pointer<Utf8>);
typedef _VoidNative = Void Function();
typedef _VoidDart = void Function();

/// The FFI surface of libihs_godot_view.so (plugin/include/.../
/// ihs_godot_view.h).
///
/// Loaded with [DynamicLibrary.open] rather than `@Native` assets: under
/// ivi-homescreen the library sits in the bundle's `lib/`, staged there by
/// emb, not by a Dart build hook. Search order:
///   1. `IHS_GODOT_VIEW_LIB`
///   2. `<executable dir>/lib/`, the bundle layout
///   3. `lib/` relative to the working directory (`homescreen -b .`)
///   4. the loader's search path
class GodotNative {
  GodotNative._(this._lib) {
    register = _lib.lookupFunction<Int32 Function(), int Function()>(
        'ihs_godot_view_register');
    unregister =
        _lib.lookupFunction<_VoidNative, _VoidDart>('ihs_godot_view_unregister');
    _setListener = _lib.lookupFunction<
        Void Function(Pointer<NativeFunction<_ListenerNative>>),
        void Function(Pointer<NativeFunction<_ListenerNative>>)>(
        'ihs_godot_view_set_listener');
    _freeString = _lib.lookupFunction<Void Function(Pointer<Utf8>),
        void Function(Pointer<Utf8>)>('ihs_godot_view_free_string');
    pointer = _lib.lookupFunction<
        Void Function(Int32, Int32, Int32, Int32, Double, Double, Double,
            Double, Int32, Double, Double, Double),
        void Function(int, int, int, int, double, double, double, double, int,
            double, double, double)>('ihs_godot_view_pointer');
    key = _lib.lookupFunction<Void Function(Uint32, Uint32, Int32, Int32, Uint32),
        void Function(int, int, int, int, int)>('ihs_godot_view_key');
    _joyConnection = _lib.lookupFunction<
        Void Function(Int32, Int32, Pointer<Utf8>),
        void Function(int, int, Pointer<Utf8>)>('ihs_godot_view_joy_connection');
    joyButton = _lib.lookupFunction<Void Function(Int32, Uint32, Int32),
        void Function(int, int, int)>('ihs_godot_view_joy_button');
    joyAxis = _lib.lookupFunction<Void Function(Int32, Uint32, Double),
        void Function(int, int, double)>('ihs_godot_view_joy_axis');
    _postMessage = _lib.lookupFunction<Void Function(Pointer<Utf8>),
        void Function(Pointer<Utf8>)>('ihs_godot_view_post_message');
    _setPaused = _lib.lookupFunction<Void Function(Int32), void Function(int)>(
        'ihs_godot_view_set_paused');

    _listener = NativeCallable<_ListenerNative>.listener(_onEvent);
    _setListener(_listener.nativeFunction);
  }

  static GodotNative? _instance;

  /// The process-wide binding; throws [GodotLoadException] when the library
  /// cannot be found.
  static GodotNative get instance => _instance ??= GodotNative._(_open());

  static DynamicLibrary _open() {
    const name = 'libihs_godot_view.so';
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    final candidates = <String>[
      if (Platform.environment['IHS_GODOT_VIEW_LIB'] case final String p) p,
      '$exeDir/lib/$name',
      'lib/$name',
      name,
    ];
    final errors = <String>[];
    for (final path in candidates) {
      try {
        return DynamicLibrary.open(path);
      } on ArgumentError catch (e) {
        errors.add('$path: ${e.message}');
      }
    }
    throw GodotLoadException(errors);
  }

  final DynamicLibrary _lib;
  late final NativeCallable<_ListenerNative> _listener;
  final _events = StreamController<GodotViewEvent>.broadcast();

  late final int Function() register;
  late final void Function() unregister;
  late final void Function(Pointer<NativeFunction<_ListenerNative>>)
      _setListener;
  late final void Function(Pointer<Utf8>) _freeString;
  late final void Function(int viewId, int kind, int phase, int pointer,
      double x, double y, double dx, double dy, int buttons, double pressure,
      double scrollDx, double scrollDy) pointer;
  late final void Function(
      int usbHidUsage, int unicode, int pressed, int repeat, int modifiers) key;
  late final void Function(int, int, Pointer<Utf8>) _joyConnection;
  late final void Function(int device, int button, int pressed) joyButton;
  late final void Function(int device, int axis, double value) joyAxis;
  late final void Function(Pointer<Utf8>) _postMessage;
  late final void Function(int) _setPaused;

  /// Every event from every view.
  Stream<GodotViewEvent> get events => _events.stream;

  void _onEvent(int viewId, int type, Pointer<Utf8> payload) {
    final text = payload == nullptr ? '' : payload.toDartString();
    if (payload != nullptr) {
      _freeString(payload);
    }
    final t = type >= 0 && type < GodotEventType.values.length
        ? GodotEventType.values[type]
        : GodotEventType.error;
    _events.add(GodotViewEvent(viewId, t, text));
  }

  void joyConnection(int device, {required bool connected, String? name}) {
    final n = (name ?? 'Flutter gamepad').toNativeUtf8();
    try {
      _joyConnection(device, connected ? 1 : 0, n);
    } finally {
      malloc.free(n);
    }
  }

  void postMessage(String text) {
    final p = text.toNativeUtf8();
    try {
      _postMessage(p);
    } finally {
      malloc.free(p);
    }
  }

  void setPaused(bool paused) => _setPaused(paused ? 1 : 0);
}

class GodotLoadException implements Exception {
  GodotLoadException(this.attempts);

  final List<String> attempts;

  @override
  String toString() =>
      'libihs_godot_view.so could not be loaded:\n  ${attempts.join('\n  ')}';
}
