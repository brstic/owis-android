// Plain-language safety notes shown before a function that changes or moves something.
// Matched on the control unit and function names (English and the original German), so they work with
// every data pack. They are reminders, not a substitute for the workshop manual of the car.
#pragma once
#include <string>
#include <vector>

namespace pt {

inline std::string lowerAscii(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }
inline bool hasAny(const std::string &hay, std::initializer_list<const char *> keys) {
    for (const char *k : keys) if (hay.find(k) != std::string::npos) return true;
    return false;
}

// cat: 'A' actuator test, 'R' routine / basic setting, 'W' write, 'J' guided job.
inline std::string safetyNote(char cat, const std::string &moduleKey, const std::string &moduleName, const std::string &function) {
    std::string t = lowerAscii(moduleKey + " " + moduleName + " " + function);
    std::vector<std::string> n;
    if (hasAny(t, {"airbag", "srs", "gurtstraffer", "belt tensioner", "pretensioner", "restraint"}))
        n.push_back("RESTRAINT SYSTEM: work on airbags and belt tensioners only as the workshop manual describes. "
                    "Nobody sits in the car. Never apply test equipment to an igniter.");
    if (hasAny(t, {"hochvolt", "high voltage", "high-voltage", "hv-", "hv_", "hybrid", "e-maschine", "e-machine", "leistungselektronik",
                   "power electronics", "battechar", "batteenerg", "charger", "ladeger", "traction"}))
        n.push_back("HIGH VOLTAGE: only persons qualified for high-voltage vehicles may work here. "
                    "This tool never disables the high-voltage safety functions.");
    if (hasAny(t, {"bleed", "entl\xC3\xBC" "ft", "entlueft", "evakuier", "evacuat", "bef\xC3\xBCll", "befuell"}) &&
        hasAny(t, {"psm", "brake", "brems", "abs", "esp"}))
        n.push_back("BRAKE BLEEDING: the pump and valves run. Vehicle secured, bleeding device connected and the "
                    "reservoir full as the manual says. Do not drive until the pedal is firm and the routine has finished.");
    else if (hasAny(t, {"psm", "brake", "brems", "abs ", "esp"}))
        n.push_back("BRAKES: the vehicle must stand still and be secured against rolling. Check the brake pedal before driving.");
    if (hasAny(t, {"epb", "parking brake", "parkbremse", "feststellbremse", "parkibrake"}))
        n.push_back("PARKING BRAKE: the brake motors move and the parking brake may release. Chock the wheels, "
                    "level ground, keep hands away from the calipers.");
    if (hasAny(t, {"pasm", "niveau", "level", "luftfeder", "air spring", "air suspension", "chasscontr", "fahrwerk", "height",
                   "h\xC3\xB6he", "hoehe", "pdcc", "roll stabil", "wankstabil", "lift_va", "lift "}))
        n.push_back("SUSPENSION: the car can rise, drop or tilt. Nobody under or beside the car, nothing in the wheel "
                    "arches, level ground. Use a lift only if the manual says so for this step.");
    if (hasAny(t, {"lenk", "steer", "ephs", "eps"}))
        n.push_back("STEERING: the wheels can turn by themselves. Hands off the steering wheel, front wheels free.");
    if (hasAny(t, {"kraftstoff", "fuel", "einspritz", "inject", "z\xC3\xBCnd", "zuend", "ignition", "tankentl", "purge"}))
        n.push_back("FUEL / IGNITION: no open flame or sparks, good ventilation. The component is operated for real.");
    if (hasAny(t, {"dme", "ecm", "motor", "engine", "med17", "drossel", "throttle", "adaption", "adaptation", "lernwert"}) && (cat == 'R' || cat == 'W' || cat == 'J'))
        n.push_back("ENGINE: after resetting adaptations the engine has to relearn - idle can be uneven for a while. "
                    "Keep clear of belts and fans if the engine has to run.");
    if (hasAny(t, {"pdk", "getriebe", "transmission", "kupplung", "clutch", "allrad", "all-wheel", "allwheel", "quersperre", "differential"}))
        n.push_back("DRIVETRAIN: selector in P, parking brake on, nobody in front of or behind the car. "
                    "Some procedures need a defined oil temperature - see the manual.");
    if (hasAny(t, {"verdeck", "roof", "spoiler", "aerod", "fenster", "window", "heckklappe", "tailgate", "liftgate", "t\xC3\xBCr", "door",
                   "sunro", "schiebedach", "wisch", "wiper", "sitz", "seat", "spiegel", "mirror", "klappe", "flap"}))
        n.push_back("MOVING PARTS: roof, spoiler, windows, seats, wipers or flaps can move. Keep hands and tools clear.");
    if (hasAny(t, {"l\xC3\xBC" "fter", "luefter", "fan", "gebl\xC3\xA4se", "geblaese", "blower", "kompressor", "compressor", "pumpe", "pump"}))
        n.push_back("ROTATING PARTS: fans, pumps or the compressor can start without warning.");
    if (hasAny(t, {"batter", "bms", "energiemanag", "energy manag"}) && !hasAny(t, {"hochvolt", "high voltage", "hybrid"}))
        n.push_back("BATTERY: register a battery only when one was really fitted - wrong data shortens battery life.");
    if (hasAny(t, {"scheinwerfer", "headl", "leuchtweite", "beam", "acc", "radar", "kamera", "camera", "lane", "spurwechsel"}))
        n.push_back("CALIBRATION: needs the prescribed targets, distances and a level floor. A wrong calibration "
                    "affects headlight aim or the driver-assistance systems.");
    std::string out;
    if (cat == 'W') out = "This WRITES a value permanently into the control unit. The current value is read and logged first - keep the log so it can be put back.";
    else if (cat == 'A') out = "This operates a component for real. Press STOP to end the test and hand control back.";
    else if (cat == 'R') out = "This starts a routine in the control unit. The conditions in the workshop manual must be met.";
    else if (cat == 'J') out = "This job runs a workshop procedure in the control unit from start to finish.";
    out += "\nIgnition ON, vehicle standing still, battery charger connected.";
    for (auto &x : n) out += "\n\n" + x;
    return out;
}

} // namespace pt
