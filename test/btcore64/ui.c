/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for the module's side of the desktop's view of Bluetooth
 * (Sources/Networking/Bluetooth/c/bt_ui.c): the summary SWI's packing, the icon
 * bar icon's sprite in each state, the device entries and the menu text made
 * of them, the profile a device is connected with, and the message that
 * tells the tasks something changed.
 */
#include <stdio.h>
#include <string.h>

#include "bt_ui.h"
#include "check.h"

static bt_ui_summary make(unsigned state)
{
	bt_ui_summary s;

	memset(&s, 0, sizeof s);
	s.state = state;
	return s;
}

static void summary(void)
{
	bt_ui_summary s = make(3), t;
	uint32_t p;

	printf("the summary SWI\n");
	s.scanning = 1; s.prompt = 1; s.audio = 2; s.reconnecting = 1;
	s.connected = 1; s.power_wanted = 1; s.settings_read = 1;
	p = btui_pack_summary(&s);
	btui_unpack_summary(p, 1234, 0x0b44, &t);
	CHECK("every field survives the packing", t.state == 3 && t.scanning &&
	      t.prompt && t.audio == 2 && t.reconnecting && t.connected &&
	      t.power_wanted && t.settings_read);
	CHECK("with the generation and the last problem beside it",
	      t.generation == 1234 && t.problem == 0x0b44);
	s = make(5);
	p = btui_pack_summary(&s);
	btui_unpack_summary(p, 0, 0, &t);
	CHECK("a failed core is state 5 and nothing else", t.state == 5 && !t.scanning &&
	      !t.connected && t.audio == 0);
	CHECK("the packed word fits the bits the header names",
	      (btui_pack_summary(&s) & ~0xfffu) == 0);
}

static void sprites(void)
{
	bt_ui_summary s;

	printf("the icon bar icon\n");
	s = make(0);
	CHECK("off: idle", strcmp(btui_sprite(&s), "bt_idle") == 0);
	s = make(5);
	CHECK("failed: idle too", strcmp(btui_sprite(&s), "bt_idle") == 0);
	s = make(1);
	CHECK("starting: the orange busy icon", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(2);
	CHECK("configuring: busy", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(4);
	CHECK("stopping: busy", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(3);
	CHECK("ready, nothing connected: the plain icon (bt_ready, the icon bar's cropped copy of !bluetooth)", strcmp(btui_sprite(&s), "bt_ready") == 0);
	s.connected = 1;
	CHECK("a device connected: the on icon", strcmp(btui_sprite(&s), "bt_on") == 0);
	s.scanning = 1;
	CHECK("scanning is busy even with a device connected", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(3); s.connected = 1; s.reconnecting = 1;
	CHECK("devices being brought back: busy", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(3); s.audio = 1;
	CHECK("a stream being set up: busy", strcmp(btui_sprite(&s), "bt_busy") == 0);
	s = make(3); s.connected = 1; s.audio = 2;
	CHECK("streaming is not busy", strcmp(btui_sprite(&s), "bt_on") == 0);
	s = make(3); s.prompt = 1;
	CHECK("a pairing question waiting: busy", strcmp(btui_sprite(&s), "bt_busy") == 0);
}

static void devices(void)
{
	uint8_t entry[BTUI_ENTRY_BYTES];
	bt_ui_device d[3];
	char text[256];
	size_t n;

	printf("the devices\n");
	memset(entry, 0, sizeof entry);
	entry[0] = 0x11; entry[5] = 0x66; entry[6] = 0x20 | 0x04 | 0x80; entry[7] = 0x01;
	entry[9] = 1; entry[13] = 2; entry[82] = 0xff;
	strcpy((char *)entry + 16, "MX Master 3S");
	btui_read_entry(entry, &d[0]);
	CHECK("an entry is read: address, flags, type, link, name",
	      d[0].address[0] == 0x11 && d[0].address[5] == 0x66 &&
	      d[0].flags == (0x20 | 0x04 | 0x80 | 0x100) && d[0].address_type == 1 &&
	      d[0].link_state == 2 && strcmp(d[0].name, "MX Master 3S") == 0);
	CHECK("a Low Energy device connects as profile 3", btui_profile_for(&d[0]) == 3);
	d[1] = d[0]; d[1].flags = BTUI_DEV_AUDIO_SINK | BTUI_DEV_PAIRED;
	strcpy(d[1].name, "Speaker, \"big\"");
	CHECK("speakers are A2DP (1)", btui_profile_for(&d[1]) == 1);
	d[2] = d[0]; d[2].flags = BTUI_DEV_HID | BTUI_DEV_AUDIO_SINK;
	d[2].name[0] = 0;
	CHECK("a headset with buttons is classic HID (2)", btui_profile_for(&d[2]) == 2);

	n = btui_devices_description(d, 3, 12, "No devices", text, sizeof text);
	CHECK("three entries", n == 3);
	CHECK("quoted, comma separated; a comma or quote in a name made harmless",
	      strcmp(text, "\"MX Master 3S\",\"Speaker   big \",\"Audio device 66:00:00:00:00:11\"") == 0);
	n = btui_devices_description(d, 3, 2, "No devices", text, sizeof text);
	CHECK("no more than asked for", n == 2 && strstr(text, "Audio device") == NULL);
	{
		bt_ui_device u;
		char label[80];

		memset(&u, 0, sizeof u);
		u.address[5] = 0xaa; u.address[4] = 0xbb; u.address[0] = 0x01;
		u.flags = BTUI_DEV_KEYBOARD | BTUI_DEV_HID;
		btui_label(&u, label, sizeof label);
		CHECK("no name: a keyboard is labelled by kind and address",
		      strcmp(label, "Keyboard aa:bb:00:00:00:01") == 0);
		u.flags = BTUI_DEV_MOUSE;
		btui_label(&u, label, sizeof label);
		CHECK("a mouse", strncmp(label, "Mouse ", 6) == 0);
		u.flags = BTUI_DEV_AUDIO_SINK;
		btui_label(&u, label, sizeof label);
		CHECK("a speaker", strncmp(label, "Audio device ", 13) == 0);
		u.flags = BTUI_DEV_LE;
		btui_label(&u, label, sizeof label);
		CHECK("an unknown Low Energy device", strncmp(label, "Bluetooth LE device ", 20) == 0);
		u.flags = 0;
		btui_label(&u, label, 8);
		CHECK("a small buffer is filled to its end, never past it",
		      strlen(label) == 7);
		strcpy(u.name, "K380");
		btui_label(&u, label, sizeof label);
		CHECK("a name is shown as it is", strcmp(label, "K380") == 0);
	}
	n = btui_devices_description(d, 0, 12, "No devices", text, sizeof text);
	CHECK("no devices: one shaded entry saying so", n == 1 && strcmp(text, "~\"No devices\"") == 0);
	n = btui_devices_description(d, 3, 12, "x", text, 12);
	CHECK("a small buffer is filled to its end, never past it", strlen(text) < 12);
}

static void message(void)
{
	uint32_t b[7];

	printf("the change message\n");
	btui_message_changed(b, 77, 0x1234);
	CHECK("a user message of 28 bytes, action 0x5BC40, the generation and the summary",
	      b[0] == 28 && b[4] == 0x5BC40u && b[5] == 77 && b[6] == 0x1234 &&
	      b[1] == 0 && b[2] == 0 && b[3] == 0);
}

static void files(void)
{
	bt_ui_config c;
	char text[96], path[80];
	uint8_t raw[6];
	static const char *lines[] = { "Power=1", "Start=0", "Discoverable=1", "Reconnect=0", "Output=1" };
	unsigned i;

	printf("the files the module reads\n");
	btui_config_defaults(&c);
	CHECK("defaults: off, started with the desktop, reconnecting, not visible",
	      !c.power && c.start && !c.discoverable && c.reconnect && !c.output);
	for (i = 0; i < 5; i++)
		CHECK("every Config line is understood", btui_config_line(lines[i], &c));
	CHECK("and sets its flag", c.power && !c.start && c.discoverable && !c.reconnect && c.output);
	CHECK("an unknown line is not", !btui_config_line("Colour=1", &c));
	CHECK("the file is written back as it was read",
	      btui_config_text(&c, text, sizeof text) > 0 &&
	      strcmp(text, "Power=1\nStart=0\nDiscoverable=1\nReconnect=0\nOutput=1\n") == 0);
	c.idle = 0;
	CHECK("an Idle line is written only when it is not the default (8)",
	      btui_config_text(&c, text, sizeof text) > 0 &&
	      strstr(text, "Output=1\nIdle=0\n") != NULL);
	CHECK("and read back", btui_config_line("Idle=15", &c) && c.idle == 15);
	{
		bt_ui_config d;

		btui_config_defaults(&d);
		(void)btui_config_text(&d, text, sizeof text);
		CHECK("the defaults file is the app's old format too",
		      strcmp(text, "Power=0\nStart=1\nDiscoverable=0\nReconnect=1\nOutput=0\n") == 0);
	}

	CHECK("an address, most significant first, is stored least significant first",
	      btui_parse_address("AA:BB:CC:DD:EE:0F", raw) == 17 && raw[0] == 0x0f && raw[5] == 0xaa);
	CHECK("lower case is fine", btui_parse_address("aa:bb:cc:dd:ee:ff", raw) == 17);
	CHECK("a short or wrong one is refused", btui_parse_address("AA:BB:CC", raw) == 0 &&
	      btui_parse_address("AA-BB-CC-DD-EE-FF", raw) == 0);

	CHECK("a button line: 4 and a path", btui_button_line("4 Resources:$.Apps.!Edit\n", path, sizeof path) == 4 &&
	      strcmp(path, "Resources:$.Apps.!Edit") == 0);
	CHECK("button 5 with spaces", btui_button_line("  5   Boot:Apps.!Paint \r\n", path, sizeof path) == 5 &&
	      strcmp(path, "Boot:Apps.!Paint") == 0);
	CHECK("None means unmapped", btui_button_line("6 None", path, sizeof path) == 0);
	CHECK("buttons 1 to 3 are the pointer's own", btui_button_line("3 !Edit", path, sizeof path) == 0);
	CHECK("a comment or nonsense is not a mapping", btui_button_line("| mapping", path, sizeof path) == 0 &&
	      btui_button_line("4", path, sizeof path) == 0 && btui_button_line("40 x", path, sizeof path) == 0);
	CHECK("a path too long for the room is refused", btui_button_line("4 0123456789", path, 5) == 0);
}

/* The fault of sd113/sd114: the module never found Choices, so it kept no
 * settings and no bonds. Choices$Path is a long string ("<ChoicesRW>.,
 * Boot:RO1000Hook."); the module read it into a 7-octet buffer, which fails
 * for a value that does not fit, and took that failure for "not set". */
static void finding_choices(void)
{
	bt_ui_settings_poll p;
	char path[80];
	uint32_t t = 5000;
	int looks = 0, loaded = 0;

	printf("finding Choices\n");
	memset(&p, 0, sizeof p);
	CHECK("the first look is due at once", btui_settings_due(&p, t));
	CHECK("Choices$Path not set: nothing is read", !btui_settings_decide(&p, 0));
	CHECK("and it is not looked at again within a second",
	      !btui_settings_due(&p, t + 500));
	CHECK("a second later it is", btui_settings_due(&p, t + 1000));
	CHECK("set now: the settings are read, once", btui_settings_decide(&p, 1));
	CHECK("never twice", !btui_settings_due(&p, t + 5000) && !btui_settings_decide(&p, 1));

	memset(&p, 0, sizeof p);
	(void)btui_settings_due(&p, t);
	p.forced = 1;			/* the desktop is starting */
	CHECK("the desktop starting forces it, at once, whatever the variable says",
	      btui_settings_due(&p, t + 1) && btui_settings_decide(&p, 0));

	/* The boot-time sequence: unset for a few seconds, then set. */
	memset(&p, 0, sizeof p);
	for (t = 0; t < 20000; t += 20) {		/* the 2 cs ticker */
		if (!btui_settings_due(&p, t))
			continue;
		looks++;
		if (btui_settings_decide(&p, t >= 7000)) {
			loaded = 1;
			break;
		}
	}
	CHECK("read within a second of Choices$Path appearing", loaded && t >= 7000 && t < 8100);
	CHECK("having looked about eight times, not at every tick", looks <= 9);

	CHECK("settings are read from Choices:Bluetooth.<leaf>",
	      btui_read_path("Config", path, sizeof path) > 0 &&
	      strcmp(path, "Choices:Bluetooth.Config") == 0);
	CHECK("and written to <Choices$Write>.Bluetooth.<leaf>, the same leaf",
	      btui_write_path("Config", path, sizeof path) > 0 &&
	      strcmp(path, "<Choices$Write>.Bluetooth.Config") == 0);
	CHECK("in a directory made first",
	      btui_write_dir(path, sizeof path) > 0 &&
	      strcmp(path, "<Choices$Write>.Bluetooth") == 0);
	CHECK("a path too long for the room gives nothing, not a cut name",
	      btui_write_path("Keys", path, 10) == 0 && path[0] == 0);
}

static void batteries(void)
{
	uint8_t e[BTUI_ENTRY_BYTES];
	bt_ui_device d[2];
	char text[200];

	printf("battery levels in the device list\n");
	memset(e, 0, sizeof e);
	memcpy(e + 16, "MX Master", 10);
	e[6] = 0x80; e[7] = 0x01; e[82] = 83;          /* paired, connected */
	btui_read_entry(e, &d[0]);
	CHECK("the level is read from the entry", d[0].battery == 83);
	e[82] = 0xff; e[6] = 0x00; e[7] = 0x00;
	memcpy(e + 16, "Keyboard K380", 14);
	e[6] = 0x80; e[7] = 0x01;
	btui_read_entry(e, &d[1]);
	CHECK("0xff is not known", d[1].battery == 0xff);
	(void)btui_devices_description(d, 2, 4, "None", text, sizeof text);
	CHECK("the menu shows a connected device's level, and none where it is not known",
	      strcmp(text, "\"MX Master 83%\",\"Keyboard K380\"") == 0);
	d[0].flags = 0x0080;                          /* paired, away */
	(void)btui_devices_description(d, 1, 4, "None", text, sizeof text);
	CHECK("a device that is away shows no level (it may be old)",
	      strcmp(text, "\"MX Master\"") == 0);
	d[0].flags = 0x0180; d[0].battery = 100;
	(void)btui_devices_description(d, 1, 4, "None", text, sizeof text);
	CHECK("100 %", strcmp(text, "\"MX Master 100%\"") == 0);
	d[0].battery = 5;
	(void)btui_devices_description(d, 1, 4, "None", text, sizeof text);
	CHECK("5 %", strcmp(text, "\"MX Master 5%\"") == 0);
}

static void volume(void)
{
	unsigned int v, worst = 0;

	printf("volume: the mixer's gain and AVRCP's absolute volume\n");
	CHECK("full volume is 0 dB", btui_volume_to_gain(127) == 0);
	CHECK("nothing is the mixer's floor, -48 dB", btui_volume_to_gain(0) == -768);
	CHECK("the middle is about -24 dB", btui_volume_to_gain(64) > -390 &&
	      btui_volume_to_gain(64) < -370);
	CHECK("a gain above 0 dB is full volume", btui_gain_to_volume(40) == 127);
	CHECK("a gain below the floor is nothing", btui_gain_to_volume(-1000) == 0);
	for (v = 0; v <= 127; v++) {
		unsigned int back = btui_gain_to_volume(btui_volume_to_gain(v));
		unsigned int d = back > v ? back - v : v - back;

		if (d > worst) worst = d;
	}
	CHECK("a volume goes to a gain and back to the same volume", worst == 0);
	CHECK("a volume never maps outside the mixer's range",
	      btui_volume_to_gain(300) == 0);
	CHECK("a 3 dB button step is 48 units of 1/16 dB",
	      BTUI_GAIN_STEP == 48);
}

int main(void)
{
	batteries();
	volume();
	finding_choices();
	files();
	summary();
	sprites();
	devices();
	message();
	return check_summary("ui");
}
