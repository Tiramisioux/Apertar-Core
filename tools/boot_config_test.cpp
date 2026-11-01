// Host test for src/hardware/BootConfigText.hpp (the System pane's config.txt card).
//   g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/boot_config_test.cpp -o /tmp/bct && /tmp/bct
#include "hardware/BootConfigText.hpp"

#include <cstdio>
#include <string>

using namespace apertar::bootconf;

static int g_failed = 0;
static void check(bool ok, const char *what)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

// The dev Pi's /boot/firmware/config.txt on 2026-11-01 (Apertar-Beta 1.6.0, DPI overlays commented out by hand).
static const std::string kApertar = R"(# For more options and information see
# http://rptl.io/configtxt
# Some settings may impact device functionality. See link above for details

#-------------------------------------------------------------------------------------#
#- Pi4 Specific options -#
#-------------------------------------------------------------------------------------#
[pi4]
disable_poe_fan=1
force_eeprom_read=0
dtoverlay=imx477

#-------------------------------------------------------------------------------------#
#- CM4 Specific options -#
#-------------------------------------------------------------------------------------#
[cm4]
otg_mode=1

#-------------------------------------------------------------------------------------#
#- CM5 Specific options -#
#-------------------------------------------------------------------------------------#
[cm5]
dtoverlay=dwc2,dr_mode=host

#-------------------------------------------------------------------------------------#
#- Pi5 Specific options -#
#-------------------------------------------------------------------------------------#
[pi5]
# Uncomment some or all of these to enable the optional hardware interfaces
dtparam=i2c_arm=on
#dtparam=i2s=on
#dtparam=spi=on
dtparam=i2c_csi_dsi0=on
dtparam=i2c_csi_dsi1=on
dtparam=pciex1
dtparam=pciex1_gen=3

#IMPORTANT! If your camera module doesn't get detected by the script, remove the "include camera-selection.txt" line
#and uncomment (remove the #) the dtoverlay for your sensor below:

#dtoverlay=imx477,cam0,link-frequency=945000000
dtoverlay=imx585,cam0,link-frequency=891000000
#dtoverlay=imx294,cam0

#-------------------------------------------------------------------------------------#
#- Common options -#
#-------------------------------------------------------------------------------------#
[all]
usb_max_current_enable=1
avoid_warnings=1
disable_splash=1
boot_delay=0
power_off_on_halt=1
camera_auto_detect=0
auto_initramfs=1
dtparam=audio=on
display_auto_detect=1
display_default_lcd=1
dtoverlay=vc4-kms-v3d
max_framebuffers=2
disable_fw_kms_setup=1
arm_64bit=1
disable_overscan=1
arm_boost=1
dtoverlay=rp1-clk-333mhz

#-------------------------------------------------------------------------------------#
#- DPI Displays -#
#-------------------------------------------------------------------------------------#
#Supported DPI Display's - Uncomment the dtoverlays for the one you are currently using.

#-- Hyperpixel 4Inch Square DPI Display --#
#dtoverlay=vc4-kms-dpi-hyperpixel4sq

#-- Waveshare 4Inch Square DPI Display --#
#dtoverlay=vc4-kms-DPI-4inch
#dtoverlay=waveshare-4dpi
#dtoverlay=waveshare-touch-4dpi

)";

static Board pi5() { return boardFromDeviceTree(std::string("raspberrypi,5-model-b\0brcm,bcm2712\0", 35), "Raspberry Pi 5 Model B Rev 1.0"); }
static Board pi4() { return boardFromDeviceTree(std::string("raspberrypi,4-model-b\0brcm,bcm2711\0", 35), "Raspberry Pi 4 Model B Rev 1.4"); }

static Facts facts(const Board &board) { return Facts{board, std::nullopt}; }

// A request that keeps everything as the file has it.
static Request same(const State &s)
{
    Request r;
    for (const auto &port : boardPorts(s.board)) {
        r.sensors[port] = s.cameras.at(port).sensor;
        r.linkFrequencies[port] = s.cameras.at(port).linkFrequency;
    }
    for (const auto &[key, t] : s.toggles) r.toggles[key] = t.on;
    r.rp1 = s.rp1.on;
    r.display = s.display;
    return r;
}

static std::string lineOf(const std::string &text, size_t number)
{
    size_t start = 0;
    for (size_t i = 1; i < number; ++i) {
        start = text.find('\n', start);
        if (start == std::string::npos) return {};
        ++start;
    }
    const size_t end = text.find('\n', start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static size_t countLines(const std::string &text)
{
    size_t n = 0;
    for (const char ch : text) n += ch == '\n';
    return n + (!text.empty() && text.back() != '\n');
}

int main()
{
    const Board board5 = pi5();
    check(board5.family == "pi5" && board5.detected && !board5.computeModule, "bcm2712 is a Pi 5");
    check(boardFromDeviceTree("", "").family == "pi5" && !boardFromDeviceTree("", "").detected, "no device tree reads as an undetected Pi 5");
    check(boardFromDeviceTree(std::string("raspberrypi,5-compute-module\0brcm,bcm2712\0", 42), "Raspberry Pi Compute Module 5 Rev 1.0").computeModule,
          "a CM5 is a compute module");

    // ---- reading the Apertar file on a Pi 5 ----
    const State s = readState(parseDocument(kApertar, board5), board5);
    check(s.editable && s.problem.empty(), "the Apertar file is editable");
    check(s.cameras.at("cam0").sensor == "imx585" && s.cameras.at("cam0").line == 42 &&
          s.cameras.at("cam0").linkFrequency == 891000000ull, "cam0 is the IMX585 at 891 MHz on line 42");
    check(s.cameras.at("cam1").sensor == "none", "cam1 is empty");
    check(s.toggles.at("i2c").on && s.toggles.at("i2c").line == 30, "I2C on, line 30");
    check(!s.toggles.at("i2s").on && !s.toggles.at("spi").on, "I2S and SPI off");
    check(s.toggles.at("audio").on && s.toggles.at("audio").line == 56, "audio on, line 56");
    check(s.rp1.on && s.rp1.line == 65 && s.rp1Overlay == "rp1-clk-333mhz", "RP1 clock on, line 65");
    check(s.display == "hdmi" && s.dpiOverlays.empty(), "DPI overlays all commented: HDMI");
    check(s.cameraAutoDetect == 0, "camera_auto_detect=0");
    check(s.candidateLinks.at("cam0").at("imx477") == 945000000ull, "the commented IMX477 line's 945 MHz is a candidate");
    check(s.warnings.empty(), "no warnings");

    // ---- on a Pi 4 the [pi4] lines count and the [pi5] ones do not ----
    const State s4 = readState(parseDocument(kApertar, pi4()), pi4());
    check(s4.cameras.at("cam1").sensor == "imx477" && s4.cameras.at("cam1").line == 11, "Pi 4: the IMX477 on line 11 (cam1, the one connector)");
    check(s4.cameras.at("cam0").sensor == "none" && !s4.toggles.at("i2c").on, "Pi 4: the [pi5] IMX585 and I2C lines do not apply");
    check(!rp1Available(s4, facts(pi4())), "Pi 4: no RP1 switch");
    check(boardPorts(pi4()) == std::vector<std::string>({"cam1"}), "Pi 4: one port");

    // ---- a request equal to the file changes nothing ----
    {
        const auto r = applyRequest(kApertar, same(s), facts(board5));
        check(r.ok && r.changes.empty() && r.text == kApertar, "no-op request: same bytes");
    }

    // ---- cameras ----
    {
        Request q = same(s);
        q.sensors["cam0"] = "imx294";
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && r.changes.size() == 2, "imx294: two lines change");
        check(lineOf(r.text, 42) == "#dtoverlay=imx585,cam0,link-frequency=891000000" && lineOf(r.text, 43) == "dtoverlay=imx294,cam0",
              "imx294: line 42 commented, line 43 uncommented");
        check(r.after.cameras.at("cam0").sensor == "imx294", "imx294: read back");
    }
    {
        Request q = same(s);
        q.sensors["cam0"] = "imx477";
        q.linkFrequencies["cam0"] = 945000000ull;
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && lineOf(r.text, 41) == "dtoverlay=imx477,cam0,link-frequency=945000000" &&
              lineOf(r.text, 42) == "#dtoverlay=imx585,cam0,link-frequency=891000000", "imx477 at 945: the file's own line, uncommented");
        q.linkFrequencies["cam0"] = std::nullopt;
        const auto d = applyRequest(kApertar, q, facts(board5));
        check(d.ok && lineOf(d.text, 41) == "dtoverlay=imx477,cam0", "imx477 at its default: no link-frequency word");
    }
    {
        Request q = same(s);
        q.sensors["cam0"] = "imx585_mono";
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && r.changes.size() == 1 && lineOf(r.text, 42) == "dtoverlay=imx585,cam0,mono,link-frequency=891000000",
              "imx585 mono: the same line gets mono after the port");
        Request back = same(r.after);
        back.sensors["cam0"] = "imx585";
        const auto b = applyRequest(r.text, back, facts(board5));
        check(b.ok && b.text == kApertar, "and back to colour gives the original bytes");
    }
    {
        Request q = same(s);
        q.linkFrequencies["cam0"] = 720000000ull;
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && lineOf(r.text, 42) == "dtoverlay=imx585,cam0", "imx585 at 720 MHz (default): the word goes");
        q.linkFrequencies["cam0"] = 1039500000ull;
        const auto f = applyRequest(kApertar, q, facts(board5));
        check(f.ok && lineOf(f.text, 42) == "dtoverlay=imx585,cam0,link-frequency=1039500000", "imx585 at 1039.5 MHz: replaced in place");
        q.linkFrequencies["cam0"] = 1188000000ull;
        const auto bad = applyRequest(kApertar, q, facts(board5));
        check(!bad.ok && bad.text.empty() && bad.error.find("1188000000") != std::string::npos, "1188 MHz is refused");
    }
    {
        Request q = same(s);
        q.sensors["cam1"] = "imx585_mono";
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && r.changes.size() == 1 && r.changes[0].line == 0 && r.changes[0].after == 43 &&
              lineOf(r.text, 44) == "dtoverlay=imx585,mono", "cam1 mono: a new line after the sensor list, no port word");
        check(r.after.cameras.at("cam1").sensor == "imx585_mono" && r.after.cameras.at("cam0").sensor == "imx585", "cam1 mono: both read back");
        Request off = same(r.after);
        off.sensors["cam1"] = "none";
        const auto o = applyRequest(r.text, off, facts(board5));
        check(o.ok && lineOf(o.text, 44) == "#dtoverlay=imx585,mono", "cam1 none: commented, kept as a candidate");
    }
    {
        Request q = same(s);
        q.sensors["cam0"] = "imx577";
        Facts f{board5, std::set<std::string>{"imx477", "imx585", "imx294", "rp1-clk-333mhz"}};
        const auto missing = applyRequest(kApertar, q, f);
        check(!missing.ok && missing.error.find("imx577.dtbo") != std::string::npos, "imx577 without its overlay is refused");
        f.overlays->insert("imx577");
        const auto r = applyRequest(kApertar, q, f);
        check(r.ok && lineOf(r.text, 44) == "dtoverlay=imx577,cam0" && lineOf(r.text, 42).rfind('#', 0) == 0, "imx577 with its overlay: added after the list");
    }
    {
        Request q = same(s4);
        q.sensors["cam0"] = "imx585";
        const auto r = applyRequest(kApertar, q, facts(pi4()));
        check(!r.ok && r.error.find("cam0") != std::string::npos, "Pi 4: cam0 is refused");
        Request p = same(s4);
        p.sensors["cam1"] = "none";
        const auto n = applyRequest(kApertar, p, facts(pi4()));
        check(n.ok && lineOf(n.text, 11) == "#dtoverlay=imx477" && n.changes.size() == 1, "Pi 4: the [pi4] line is the one edited");
    }

    // ---- interfaces, RP1, display ----
    {
        Request q = same(s);
        q.toggles["i2s"] = true;
        q.toggles["spi"] = true;
        q.toggles["i2c"] = false;
        q.toggles["audio"] = false;
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && r.changes.size() == 4, "four switches: four lines");
        check(lineOf(r.text, 30) == "#dtparam=i2c_arm=on" && lineOf(r.text, 31) == "dtparam=i2s=on" &&
              lineOf(r.text, 32) == "dtparam=spi=on" && lineOf(r.text, 56) == "#dtparam=audio=on", "switches: comment and uncomment in place");
        const auto back = applyRequest(r.text, same(s), facts(board5));
        check(back.ok && back.text == kApertar, "switches back: the original bytes");
    }
    {
        Request q = same(s);
        q.rp1 = false;
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && lineOf(r.text, 65) == "#dtoverlay=rp1-clk-333mhz", "RP1 off: line 65 commented");
        Request on = same(r.after);
        on.rp1 = true;
        const auto missing = applyRequest(r.text, on, Facts{board5, std::set<std::string>{"imx585"}});
        check(!missing.ok && missing.error.find("rp1-clk-333mhz.dtbo") != std::string::npos, "RP1 on without the overlay is refused");
        const auto again = applyRequest(r.text, on, facts(board5));
        check(again.ok && again.text == kApertar, "RP1 on again: the original bytes");
        Request pi4On = same(s4);
        pi4On.rp1 = true;
        check(applyRequest(kApertar, pi4On, facts(pi4())).ok, "Pi 4: RP1 already reads on from [all], nothing to do");
    }
    {
        Request q = same(s);
        q.display = "waveshare4";
        const auto r = applyRequest(kApertar, q, facts(board5));
        check(r.ok && r.displayChanged && r.changes.size() == 3 && lineOf(r.text, 76) == "dtoverlay=vc4-kms-DPI-4inch" &&
              lineOf(r.text, 77) == "dtoverlay=waveshare-4dpi" && lineOf(r.text, 78) == "dtoverlay=waveshare-touch-4dpi", "Waveshare: lines 76-78 uncommented");
        check(r.after.display == "waveshare4", "Waveshare: read back");
        Request hp = same(r.after);
        hp.display = "hyperpixel4sq";
        const auto h = applyRequest(r.text, hp, facts(board5));
        check(h.ok && lineOf(h.text, 73) == "dtoverlay=vc4-kms-dpi-hyperpixel4sq" && lineOf(h.text, 76) == "#dtoverlay=vc4-kms-DPI-4inch",
              "Hyperpixel: line 73 on, the Waveshare lines off");
        Request hdmi = same(h.after);
        hdmi.display = "hdmi";
        const auto b = applyRequest(h.text, hdmi, facts(board5));
        check(b.ok && b.text == kApertar, "HDMI again: the original bytes");
        check(!applyRequest(kApertar, same(s), facts(board5)).displayChanged, "no display change, no flag");
    }

    // ---- other files ----
    {
        const std::string fan = kApertar + "# BEGIN APERTAR FAN MODE\n# mode: Silent\ndtparam=cooling_fan=on\ndtparam=fan_temp0=60000\n# END APERTAR FAN MODE\n";
        Request q = same(s);
        q.toggles["audio"] = false;
        q.display = "waveshare4";
        const auto r = applyRequest(fan, q, facts(board5));
        check(r.ok && r.text.find("# BEGIN APERTAR FAN MODE\n# mode: Silent\ndtparam=cooling_fan=on\ndtparam=fan_temp0=60000\n# END APERTAR FAN MODE\n") != std::string::npos &&
              r.changes.size() == 4, "the fan block is kept byte for byte");
        const std::string bad = kApertar + "# BEGIN APERTAR FAN MODE\ndtoverlay=imx219\n# END APERTAR FAN MODE\n";
        const State sb = readState(parseDocument(bad, board5), board5);
        check(sb.cameras.at("cam1").sensor == "imx219", "a sensor line inside a block still counts (cam1: imx219)");
        Request none = same(sb);
        none.sensors["cam1"] = "none";
        const auto refused = applyRequest(bad, none, facts(board5));
        check(!refused.ok && refused.error.find("imx219") != std::string::npos, "a line this code may not edit decides: refused, with the line");
    }
    {
        const std::string list = "[all]\ndtparam=i2c_arm=on,i2c_arm_baudrate=400000\ndtparam=audio=on\n";
        const State sl = readState(parseDocument(list, board5), board5);
        check(sl.editable && sl.toggles.at("i2c").on, "comma list: I2C reads on");
        Request q = same(sl);
        q.toggles["i2c"] = false;
        const auto r = applyRequest(list, q, facts(board5));
        check(r.ok && lineOf(r.text, 2) == "dtparam=i2c_arm=off,i2c_arm_baudrate=400000", "comma list: the item turns off, the baud rate stays");
        Request on = same(sl);
        on.toggles["i2s"] = true;
        on.sensors["cam0"] = "imx585";
        const auto added = applyRequest(list, on, facts(board5));
        check(added.ok && lineOf(added.text, 2) == "dtparam=i2c_arm=on,i2c_arm_baudrate=400000" && lineOf(added.text, 3) == "dtparam=i2s=on" &&
              lineOf(added.text, 5) == "dtoverlay=imx585,cam0", "no [pi5]: new lines go into [all], next to their kind");
    }
    {
        const std::string plain = "dtparam=audio=on\ncamera_auto_detect=1\n";
        const State sp = readState(parseDocument(plain, board5), board5);
        check(!sp.editable && !applyRequest(plain, same(sp), facts(board5)).ok, "no [pi5] and no [all]: not the Apertar layout, refused");
        const std::string auto1 = "[pi5]\ndtoverlay=imx585,cam0\n[all]\ncamera_auto_detect=1\n";
        const State sa = readState(parseDocument(auto1, board5), board5);
        Request q = same(sa);
        q.sensors["cam0"] = "imx477";
        const auto r = applyRequest(auto1, q, facts(board5));
        check(r.ok && lineOf(r.text, 5) == "camera_auto_detect=0" && lineOf(r.text, 2) == "#dtoverlay=imx585,cam0" &&
              lineOf(r.text, 3) == "dtoverlay=imx477,cam0", "camera_auto_detect=1 becomes 0 when a sensor is named");
    }
    {
        std::string crlf;
        for (const char ch : std::string("[pi5]\ndtparam=i2c_arm=on\n#dtparam=spi=on\ndtoverlay=imx585,cam0\n[all]\ndtparam=audio=on\n")) {
            if (ch == '\n') crlf += '\r';
            crlf += ch;
        }
        const State sc = readState(parseDocument(crlf, board5), board5);
        Request q = same(sc);
        q.toggles["spi"] = true;
        q.sensors["cam1"] = "imx294";
        const auto r = applyRequest(crlf, q, facts(board5));
        check(r.ok && lineOf(r.text, 3) == "dtparam=spi=on\r" && lineOf(r.text, 5) == "dtoverlay=imx294\r", "CRLF kept, also on an added line");
    }
    {
        const std::string twice = "[pi5]\ndtoverlay=imx585,cam0\ndtoverlay=imx477,cam0\n[all]\n";
        const State st = readState(parseDocument(twice, board5), board5);
        check(st.cameras.at("cam0").extraLines.size() == 1 && !st.warnings.empty(), "two sensors on cam0: a warning");
        const auto r = applyRequest(twice, same(st), facts(board5));
        check(r.ok && lineOf(r.text, 3) == "#dtoverlay=imx477,cam0" && r.after.cameras.at("cam0").extraLines.empty(), "saving keeps one");
    }

    // ---- the file edited as text ----
    {
        check(lineChanges(kApertar, kApertar).empty(), "text: no edit, no changes");
        std::string edited = kApertar;
        edited.replace(edited.find("dtparam=pciex1_gen=3\n"), 21, "dtparam=pciex1_gen=2\n");
        auto c = lineChanges(kApertar, edited);
        check(c.size() == 1 && c[0].line == 36 && c[0].before == "dtparam=pciex1_gen=3" && c[0].after_text == "dtparam=pciex1_gen=2" && !c[0].removed,
              "text: a changed line, by its number");
        edited = kApertar;
        edited.erase(edited.find("dtparam=pciex1\n"), 15);
        c = lineChanges(kApertar, edited);
        check(c.size() == 1 && c[0].line == 35 && c[0].removed, "text: a removed line");
        edited = kApertar + "usb_max_current_enable=1\n";
        c = lineChanges(kApertar, edited);
        check(c.size() == 1 && c[0].line == 0 && c[0].after == countLines(kApertar) && c[0].after_text == "usb_max_current_enable=1",
              "text: an added line at the end, after the last old line");
        edited = kApertar;
        edited.insert(edited.find("dtparam=pciex1\n"), "dtparam=nvme\n");
        c = lineChanges(kApertar, edited);
        check(c.size() == 1 && c[0].line == 0 && c[0].after == 34, "text: an added line in the middle, after line 34");
        check(changesJson(lineChanges(kApertar, kApertar.substr(0, kApertar.find("dtparam=pciex1\n")) +
                                                   kApertar.substr(kApertar.find("dtparam=pciex1\n") + 15)))
                  .find("\"removed\":true") != std::string::npos, "text: JSON marks a removed line");

        // the card's choices go on top of the hand-edited text
        edited = kApertar + "usb_max_current_enable=1\n";
        Request q = same(s);
        q.toggles["i2s"] = true;
        const auto r = applyRequest(edited, q, facts(board5));
        c = lineChanges(kApertar, r.text);
        check(r.ok && c.size() == 2 && lineOf(r.text, 31) == "dtparam=i2s=on", "text + a choice: both in the result, two changes");
        check(!displayDiffers(s, r.after), "text without a DPI edit: no display change");
        edited = kApertar;
        edited.replace(edited.find("#dtoverlay=vc4-kms-dpi-hyperpixel4sq"), 36, "dtoverlay=vc4-kms-dpi-hyperpixel4sq");
        const State dpi = readState(parseDocument(edited, board5), board5);
        check(displayDiffers(s, dpi), "a DPI overlay uncommented by hand: a display change");
        check(textProblem(kApertar).empty() && !textProblem("").empty() && !textProblem(std::string(65537, '#')).empty() &&
                  !textProblem(std::string("a\0b", 3)).empty(), "text: empty, over 64 KiB and NUL are refused");
    }

    // ---- JSON and revision ----
    {
        const std::string json = stateJson(s, facts(board5));
        check(json.find("\"cam0\":{\"sensor\":\"imx585\",\"link_frequency\":891000000") != std::string::npos, "JSON: cam0");
        check(json.find("891 MHz · 1782 Mbps/lane · up to 60 fps at 4K — over RP1 spec") != std::string::npos, "JSON: link labels");
        check(json.find("\"rp1\":{\"on\":true,\"line\":65,\"overlay\":\"rp1-clk-333mhz\",\"available\":true}") != std::string::npos, "JSON: RP1");
        check(revisionOf(kApertar) == revisionOf(kApertar) && revisionOf(kApertar) != revisionOf(kApertar + "\n"), "revision follows the bytes");
    }

    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
