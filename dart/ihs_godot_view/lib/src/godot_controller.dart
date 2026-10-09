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
import 'dart:convert';

import 'package:flutter/foundation.dart';

import 'native.dart';

enum GodotStatus { starting, running, quit, failed }

/// Talks to the game a [GodotView] shows: state, messages both ways, pause.
///
/// One Godot runs per process (a LibGodot limit), so there is one game to
/// talk to; the controller follows whichever view it is attached to.
class GodotController extends ChangeNotifier {
  GodotController({GodotNative? native}) : _nativeOverride = native;

  final GodotNative? _nativeOverride;
  GodotNative get _native => _nativeOverride ?? GodotNative.instance;

  StreamSubscription<GodotViewEvent>? _sub;
  final _messages = StreamController<String>.broadcast();

  int? _viewId;
  GodotStatus _status = GodotStatus.starting;
  String? _surfaceKind;
  String? _api;
  String? _backend;
  String? _error;
  bool _mouseCaptured = false;

  GodotStatus get status => _status;

  /// How frames reach the screen: dmabuf, plane, vk_image, egl_image or shm.
  String? get surfaceKind => _surfaceKind;

  /// Godot's renderer: vulkan, opengl or gles.
  String? get api => _api;

  /// The ivi-homescreen backend key.
  String? get backend => _backend;
  String? get error => _error;

  /// The game captured the mouse (Input.MOUSE_MODE_CAPTURED): hide the
  /// cursor and expect relative motion.
  bool get mouseCaptured => _mouseCaptured;

  /// `IhsHost.send_message(text)` from the game.
  Stream<String> get messages => _messages.stream;

  /// Emit `IhsHost.message_received(text)` in the game.
  void sendMessage(String text) => _native.postMessage(text);

  void setPaused(bool paused) => _native.setPaused(paused);

  /// Called by [GodotView] when its platform view is created.
  void attach(int viewId) {
    _viewId = viewId;
    _sub ??= _native.events.listen(_onEvent);
  }

  void detach(int viewId) {
    if (_viewId == viewId) {
      _viewId = null;
    }
  }

  void _onEvent(GodotViewEvent e) {
    if (_viewId != null && e.viewId != _viewId && e.viewId != -1) {
      return;
    }
    switch (e.type) {
      case GodotEventType.ready:
      case GodotEventType.surface:
        _status = GodotStatus.running;
        try {
          final info = jsonDecode(e.text) as Map<String, dynamic>;
          _surfaceKind = info['kind'] as String?;
          _api = info['api'] as String?;
          _backend = info['backend'] as String?;
        } on FormatException {
          // Keep the previous values.
        }
      case GodotEventType.message:
        _messages.add(e.text);
        return;
      case GodotEventType.mouseMode:
        _mouseCaptured = e.text == '2';
      case GodotEventType.quit:
        _status = GodotStatus.quit;
      case GodotEventType.error:
        _error = e.text;
        if (_status == GodotStatus.starting) {
          _status = GodotStatus.failed;
        }
    }
    notifyListeners();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _messages.close();
    super.dispose();
  }
}
