@ Union Room prototype (room shell only -- see CLAUDE.md). Ported from
@ Emerald's UnionRoom map/scripts, but with every hook into the real Union
@ Room application (`special RunUnionRoom`, the native state machine that
@ drives other players/chat/trade/battle over RFU) removed. What's left is
@ just enough to stand in the room: the 8 placeholder "other player" object
@ events stay hidden (nothing exists yet to reveal/reposition them for real
@ connected peers), and the attendant gives an honest placeholder message
@ instead of opening the (unported) registration menu.
UnionRoom_MapScripts::
	map_script MAP_SCRIPT_ON_RESUME, UnionRoom_OnResume
	.byte 0

UnionRoom_OnResume:
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_1
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_2
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_3
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_4
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_5
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_6
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_7
	setflag FLAG_HIDE_UNION_ROOM_PLAYER_8
	removeobject LOCALID_UNION_ROOM_PLAYER_1
	removeobject LOCALID_UNION_ROOM_PLAYER_2
	removeobject LOCALID_UNION_ROOM_PLAYER_3
	removeobject LOCALID_UNION_ROOM_PLAYER_4
	removeobject LOCALID_UNION_ROOM_PLAYER_5
	removeobject LOCALID_UNION_ROOM_PLAYER_6
	removeobject LOCALID_UNION_ROOM_PLAYER_7
	removeobject LOCALID_UNION_ROOM_PLAYER_8
	end

UnionRoom_EventScript_Attendant::
	lock
	faceplayer
	message UnionRoom_Text_AttendantPlaceholder
	waitmessage
	waitbuttonpress
	release
	end

@ Permanently unreachable for now: these objects stay hidden via
@ FLAG_HIDE_UNION_ROOM_PLAYER_1..8 above, since nothing sets their
@ appearance/position for a real connected peer yet.
UnionRoom_EventScript_Player1::
UnionRoom_EventScript_Player2::
UnionRoom_EventScript_Player3::
UnionRoom_EventScript_Player4::
UnionRoom_EventScript_Player5::
UnionRoom_EventScript_Player6::
UnionRoom_EventScript_Player7::
UnionRoom_EventScript_Player8::
	end
