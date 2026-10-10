extends Node2D

var frames := 0

func _ready() -> void:
	IhsHost.message_received.connect(_on_message)
	IhsHost.send_message("ready embedded=%s display=%s" % [IhsHost.is_embedded(), DisplayServer.get_name()])

func _on_message(text: String) -> void:
	IhsHost.send_message("echo:" + text)

func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed:
		IhsHost.send_message("key:%s" % OS.get_keycode_string(event.keycode))
	elif event is InputEventScreenTouch and event.pressed:
		IhsHost.send_message("touch:%d,%d" % [event.position.x, event.position.y])
	elif event is InputEventJoypadMotion:
		IhsHost.send_message("joy:%d=%.1f" % [event.axis, event.axis_value])
