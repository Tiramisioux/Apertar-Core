#pragma once
// cam1's side of a two-camera clip name, built from the settings body cam1 reported (dual-sensor design §6.2 step 2: "each
// name is built per camera"). Pure: host-tested (cam1_names_test.cpp). The same helpers build cam0's own values in
// CameraController, so the two names read alike.
#include "ipc/JsonKeys.hpp"
#include "util/ClipName.hpp"

#include <string>
#include <string_view>

namespace apertar::cam1names {

// `settings` is cam1.settings (an object holding the settingsBody members), `sensor` cam1's sensor name. Not present
// (present=false) while cam1 has reported nothing: the primary then names cam1 from its own values, which only matters for a
// cam0-only take's number.
inline clipname::PartnerNaming partnerFrom(std::string_view settings, std::string_view sensor)
{
    using namespace clipname;
    PartnerNaming partner;
    if (settings.size() <= 2)
        return partner;
    const auto text = [&](const char *key) { return jsonkeys::stringOf(settings, key).value_or(""); };
    const auto number = [&](const char *key) { return jsonkeys::numberOf(settings, key).value_or(0); };
    partner.present = true;
    partner.fps = number("fps") > 0 ? number("fps") : 25.0;
    Values &v = partner.values;
    v.cam = "cam1";
    const std::string format = text("recording_format");
    const bool h264 = format == "h264";
    const bool processed = h264 || format == "prores";
    if (format == "mlv") v.format = "MLV";
    else if (h264) v.format = "H264";
    else if (format == "prores") v.format = "ProRes";
    else if (format == "lossydng") v.format = text("lossy_dng_ratio") == "5to1" ? "cDNG_5to1" : "cDNG_3to1";
    else v.format = "cDNG";
    v.sensor = sanitize(std::string(sensor), 12);
    std::string model = "Apertar D1";
    if (model.rfind("Apertar ", 0) == 0) model.erase(0, 8);
    v.model = sanitize(model, 16);
    if (v.model.empty()) v.model = "Apertar";
    const int h264Width = static_cast<int>(number("h264_width"));
    const int h264Height = static_cast<int>(number("h264_height"));
    v.res = processed && h264Width > 0 ? resolutionLabel(h264Width, h264Height)
                                       : resolutionLabel(static_cast<int>(number("width")), static_cast<int>(number("height")));
    v.fps = fpsLabel(partner.fps);
    if (!h264)
        v.bits = std::to_string(processed ? 10u : static_cast<unsigned>(number("raw_bit_depth"))) + "bit";
    if (format == "cdng" || format.empty())
        v.comp = text("cdng_compression") == "lossless_jpeg" ? "lossless" : "none";
    else if (format == "lossydng")
        v.comp = text("lossy_dng_ratio") == "5to1" ? "lossy5" : "lossy3";
    if (text("cdng_log") != "off" && !text("cdng_log").empty() && jsonkeys::boolOf(settings, "cdng_log_available").value_or(false))
        v.log = "Log";
    return partner;
}

} // namespace apertar::cam1names
