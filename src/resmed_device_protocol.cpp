#include "resmed_device_protocol.h"

namespace aircannect {

const char RESMED_PLATFORM_GET_PARAMS[] = "[\"PlatformIdentifier\"]";
const char RESMED_PLATFORM_FIELD[] = "PlatformIdentifier";

namespace {

const ResmedDeviceProtocol AIRSENSE11 = {
    {"[\"_PNA\",\"_SRN\",\"_SID\",\"_BID\",\"_MID\",\"_VID\"]",
     "_PNA", "_SRN", "_SID", "_BID", "_MID", "_VID"},
    {"[\"_MOP\",\"_ROP\"]", "_MOP", "_ROP", "ROP", nullptr},
    {"[\"_MHR\"]", "_MHR"},
    {"[\"_TZO\"]", "_TZO"},
};

const ResmedDeviceProtocol AIRMINI = {
    {"[\"ProductName\",\"SerialNumber\",\"ApplicationIdentifier\","
     "\"BootloaderIdentifier\",\"PlatformIdentifier\",\"VariantIdentifier\"]",
     "ProductName", "SerialNumber", "ApplicationIdentifier",
     "BootloaderIdentifier", "PlatformIdentifier", "VariantIdentifier"},
    {"[\"TherapyMode\",\"_RUNNING_MODE_REQUEST\",\"FGState\"]",
     "TherapyMode", "_RUNNING_MODE_REQUEST", nullptr, "FGState"},
    {"[\"MotorRunMeter\"]", "MotorRunMeter"},
    {nullptr, nullptr},
};

}  // namespace

const ResmedDeviceProtocol *resmed_device_protocol(ResmedDeviceModel model) {
    switch (model) {
        case ResmedDeviceModel::AirSense11: return &AIRSENSE11;
        case ResmedDeviceModel::AirMini: return &AIRMINI;
        case ResmedDeviceModel::Unknown: return nullptr;
    }
    return nullptr;
}

}  // namespace aircannect
