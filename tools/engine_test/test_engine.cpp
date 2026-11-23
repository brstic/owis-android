// End-to-end tests of the engine against the simulated car (src/sim.h) and a real generated pack.
// No adapter, no Windows:   c++ -std=c++17 -Isrc tests/test_engine.cpp -o t && ./t data/G1_de.json
#include "engine.h"
#include "sim.h"
#include "trace.h"
#include <fstream>
#include <sstream>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
using namespace pt;

static std::string slurp(const std::string &p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static bool has(const std::string &hay, const std::string &n) { return hay.find(n) != std::string::npos; }

int main(int argc, char **argv) {
    std::string packPath = argc > 1 ? argv[1] : "data/G1_de.json";
    std::string tmp = argc > 2 ? argv[2] : ".";
    Engine en; std::string err, logText;
    en.quick = true; en.reportDir = tmp;
    en.onLog = [&](const std::string &s) { logText += s + "\n"; };
    CHECK(en.loadPack(packPath, err)); if (!err.empty()) printf("%s\n", err.c_str());
    printf("pack %s: %zu modules, %zu functions, %zu jobs\n", en.pack.platform.c_str(), en.pack.mods.size(), en.functionCount(), en.pack.jobs.size());
    sim::installDemoKeys();

    // ---- 1. scan: every simulated unit, VIN, fault texts, environment data, both reports
    sim::SimBus bus; TraceWriter tw; std::string tracePath = tmp + "/test_trace.ptrace"; remove(tracePath.c_str());
    CHECK(tw.open(tracePath)); tw.attach(bus);
    en.actScanAll(bus);
    int present = 0, faults = 0; const ScanRow *pasm = nullptr, *pdk = nullptr, *sel = nullptr;
    for (auto &r : en.scan) { if (r.present) { present++; faults += (int)r.faults.size(); }
        if (r.req == 0x755) pasm = &r; if (r.req == 0x70F) pdk = &r; if (r.req == 0x753) sel = &r; }
    printf("scan: %d units answering, %d faults, VIN %s\n", present, faults, en.vin.c_str());
    CHECK(present == 8 && faults == 4);                     // 7 on CAN/ISO-TP + the door on TP 2.0
    CHECK(en.vin == sim::DEMO_VIN);
    CHECK(pasm && pasm->proto == PROTO_UDS && pasm->faults.size() == 1 && pasm->faults[0].d.code == 0x000110);
    if (pasm && !pasm->faults.empty()) { printf("  PASM fault stored: %s\n", pasm->faults[0].env.c_str());
        CHECK(has(pasm->faults[0].env, "Frequency counter 3")); CHECK(has(pasm->faults[0].env, "Mileage 123456 km"));
        CHECK(has(pasm->faults[0].env, "2012-06-15 14:30:05")); CHECK(has(pasm->faults[0].env, "12.4 V"));
        CHECK(has(pasm->faults[0].env, "snapshot: ") && has(pasm->faults[0].env, "450 mm"));     // 19 04, decoded via 22 00A0
        CHECK(has(pasm->ident, "97034301234")); }
    CHECK(pdk && pdk->proto == PROTO_KWP && pdk->faults.size() == 1 && dtcName(pdk->faults[0].d) == "P1872");
    CHECK(sel && sel->proto == PROTO_KWP && sel->faults.size() == 1);            // UDS refused -> KWP fallback
    CHECK(!en.lastTxt.empty() && !en.lastHtml.empty());
    std::string html = slurp(en.lastHtml), txt = slurp(en.lastTxt);
    CHECK(has(html, "000110") && has(html, "P1872") && has(html, sim::DEMO_VIN) && has(html, "Mileage 123456 km"));
    CHECK(has(txt, "Modules answering: 8   Faults: 4"));
    tw.close(); bus.tap = nullptr;

    // ---- 1b. unit behind the gateway on VW TP 2.0 (frame-level: channel setup, ACKs, multi-frame answers)
    {   int di = -1; for (int i = 0; i < (int)en.pack.mods.size(); i++) if (en.pack.mods[i].link == 2 && en.pack.mods[i].ids[0].first == 0x42) di = i;
        CHECK(di >= 0);
        if (di >= 0) { ScanRow row; CHECK(en.scanOne(bus, di, true, row)); en.printRow(row, false);
            CHECK(row.present && row.proto == PROTO_KWP && row.faults.size() == 1 && has(row.ident, "Tuersteuergeraet FS H07 0130"));
            CHECK(!en.pack.mods[di].addrConfirmed); }
        int none = -1; for (int i = 0; i < (int)en.pack.mods.size(); i++) if (en.pack.mods[i].link == 2 && en.pack.mods[i].ids[0].first == 0x52) none = i;
        ScanRow r2; if (none >= 0) CHECK(!en.scanOne(bus, none, false, r2));          // passenger door not fitted: no channel
    }

    // ---- 2. replay the recorded session: same result without the car
    {   ReplayBus rb; CHECK(rb.load(tracePath)); CHECK(rb.open());
        Engine e2; e2.quick = true; e2.reportDir = tmp; e2.redactVin = true; CHECK(e2.loadPack(packPath, err));
        e2.actScanAll(rb);
        int p2 = 0, f2 = 0; for (auto &r : e2.scan) if (r.present) { p2++; f2 += (int)r.faults.size(); }
        printf("replay: %d units, %d faults (%zu recorded requests)\n", p2, f2, rb.requests);
        CHECK(p2 == 8 && f2 == 4 && e2.vin == sim::DEMO_VIN);
        std::string h2 = slurp(e2.lastHtml);
        CHECK(!has(h2, sim::DEMO_VIN) && has(h2, "WP0ZZZ97ZCL******"));           // VIN redaction
        remove(e2.lastHtml.c_str()); remove(e2.lastTxt.c_str());
    }

    // ---- 3. vehicle detection: VIN -> pack; and by part numbers when no VIN is available
    {   std::vector<PackInfo> packs; PackInfo a; a.id = "9x1"; a.title = "911"; a.vin = {"991", "981"}; a.pn = {"991"}; packs.push_back(a);
        PackInfo g; CHECK(readPackHead(packPath, g)); CHECK(g.id == "G1" && !g.vin.empty()); packs.push_back(g);
        std::string how; int k = en.detectVehicle(bus, packs, how);
        printf("detect: %s -> %s\n", how.c_str(), k >= 0 ? packs[k].id.c_str() : "?");
        CHECK(k == 1);
        CHECK(en.identifyByPartNumbers(bus, packs) == "G1");
    }

    // ---- 4. module function: ride height read, levelling routine followed to its end
    std::string outText; en.onOut = [&](const std::string &s) { outText += s + "\n"; };
    int mi = en.findModule("PASM"); CHECK(mi >= 0);
    if (mi >= 0) {
        en.selectModule(mi); const pk::Module &m = en.pack.mods[mi];
        CHECK(m.saLevel == 3 && m.saBytes == 4 && m.sk == "ASAM_seedkey_PASM");
        int level = -1, hread = -1, hwrite = -1;
        for (int i = 0; i < (int)en.entries.size(); i++) { const pk::Service &s = m.svc[en.entries[i].main];
            std::vector<uint8_t> rq; std::string e; std::vector<std::pair<int,int>> in; en.pack.inputsOf(s, in);
            en.pack.buildRequest(s, std::vector<double>(in.size(), 0), rq, e);
            if (rq.size() >= 4 && rq[0] == 0x31 && rq[2] == 0xF1 && rq[3] == 0x05) level = i;
            if (rq.size() == 3 && rq[0] == 0x22 && rq[1] == 0x00 && rq[2] == 0xA0) hread = i;
            if (rq.size() >= 3 && rq[0] == 0x2E && rq[1] == 0x00 && rq[2] == 0xA0) hwrite = i; }
        CHECK(level >= 0 && hread >= 0 && hwrite >= 0);
        outText.clear(); CHECK(en.runFunction(bus, mi, hread, {})); CHECK(has(outText, "= 500 mm"));
        outText.clear(); CHECK(en.runFunction(bus, mi, level, {4}));
        CHECK(has(outText, "> 31 01 F1 05 04")); CHECK(en.entries[level].result >= 0);
        printf("%s", outText.c_str());

        // ---- 5. coding write: extended session + SecurityAccess 27 03/04 with the computed key, then read back
        outText.clear(); CHECK(en.runFunction(bus, mi, hwrite, {480}));
        printf("%s", outText.c_str());
        CHECK(has(outText, "[UNLOCKED]")); CHECK(has(outText, "> 2E 00 A0 01 E0")); CHECK(bus.car.ecus[0x755].height == 480);
        CHECK(has(outText, "= 480 mm")); CHECK(bus.car.ecus[0x755].badKeys == 0);

        // wrong key: rejected once, never retried, nothing written
        sk::keys()["ASAM_seedkey_PASM"].B ^= 0x55;
        std::vector<uint8_t> r; bus.target(0x755, 0x7BF); bus.xfer({0x10, 0x01}, r);          // lock again
        outText.clear(); CHECK(!en.runFunction(bus, mi, hwrite, {470}));
        CHECK(has(outText, "[LOCKED] key rejected")); CHECK(bus.car.ecus[0x755].badKeys == 1); CHECK(bus.car.ecus[0x755].height == 480);
        // no key available: nothing is sent to the control unit at all
        sk::keys().erase("ASAM_seedkey_PASM");
        outText.clear(); CHECK(!en.runFunction(bus, mi, hwrite, {470}));
        CHECK(has(outText, "[LOCKED] algorithm ASAM_seedkey_PASM is not in the key file")); CHECK(bus.car.ecus[0x755].badKeys == 1);
        sim::installDemoKeys(); bus.car.ecus[0x755].badKeys = 0;
    }
    // KWP2000 unit: SecurityAccess 27 01/02 with a 2-byte seed
    {   int pi = en.findModule("Getriebesteuerung_PDK"); CHECK(pi >= 0);
        if (pi >= 0) { pk::Module m = en.pack.mods[pi]; printf("PDK security level from pack: %d, seed bytes %d\n", m.saLevel, m.saBytes);
            CHECK(m.saLevel == 1 && m.saBytes == 2);
            m.sk = "ASAM_seedkey_PDK"; std::string why; bus.target(0x70F, 0x779);
            CHECK(en.securityUnlock(bus, m, PROTO_KWP, why)); CHECK(bus.car.ecus[0x70F].unlocked);
            if (!why.empty()) printf("  %s\n", why.c_str());
            m.saBytes = 4; CHECK(!en.securityUnlock(bus, m, PROTO_KWP, why)); CHECK(has(why, "not sending a key")); }
    }

    // ---- 6. guided jobs: one call from session to result; the service type and today's date are filled in
    const pk::Job *job = en.findJob("oil_service_reset"); CHECK(job);
    if (job) { outText.clear(); int before = bus.car.ecus[0x714].resets;
        std::string note = en.jobNote(*job); CHECK(!note.empty());
        CHECK(Engine::askCount(job->steps[0]) == 0);
        CHECK(en.runJob(bus, *job, {})); printf("%s", outText.c_str());
        CHECK(bus.car.ecus[0x714].resets == before + 1); CHECK(has(outText, "[JOB DONE]"));
        CHECK(has(outText, "> 31 01 04 50 04 ")); }                 // oil service = bit 2, reserved bits 0
    job = en.findJob("service_reset"); CHECK(job);
    if (job) { outText.clear(); CHECK(Engine::askCount(job->steps[0]) == 3);
        CHECK(en.runJob(bus, *job, {{1, 1, 0}})); CHECK(has(outText, "> 31 01 04 50 03 ")); }

    // PDK teach-in (KWP2000 31 20 / 33 20): followed to "finished"; on failure the unmet conditions are listed
    if (const pk::Job *ti = en.findJob("pdk_teach_in_shift_rods")) {
        outText.clear(); CHECK(en.runJob(bus, *ti, {})); printf("%s", outText.c_str());
        CHECK(has(outText, "> 31 20") && has(outText, "[OK] routine finished") && has(outText, "[JOB DONE]"));
        bus.car.ecus[0x70F].teachFail = true; outText.clear();
        CHECK(!en.runJob(bus, *ti, {})); printf("%s", outText.c_str());
        CHECK(has(outText, "[FAILED]") && has(outText, "Conditions not met"));
        CHECK(has(outText, "Stellung P") || has(outText, "lever in P"));                       // German or English pack
        CHECK(has(outText, "Systemdruck") || has(outText, "System pressure"));
        CHECK(!has(outText, "Synchronpunkt ermittelt\n"));                                       // met conditions are not listed
        bus.car.ecus[0x70F].teachFail = false;
    } else CHECK(false);
    CHECK(en.findJob("pdk_oil_fill_mode") && en.findJob("pdk_teach_in_clutches") && en.findJob("pdk_teach_in_hydraulics"));

    // byte-typed selection inputs are numbers: "All adaption values" = 0x040000 (regression)
    if (const pk::Job *ar = en.findJob("engine_adaptation_reset")) {
        const pk::JobStep &st = ar->steps[0]; std::vector<uint8_t> rq; std::string e;
        CHECK(en.pack.buildRequest(en.pack.mods[st.mod].svc[st.svc], Engine::stepInputs(st, {}), rq, e));
        printf("engine adaptation reset request: %s\n", hexStr(rq).c_str());
        CHECK(rq.size() == 7 && rq[0] == 0x31 && rq[1] == 0x01 && rq[4] == 0x04 && rq[5] == 0 && rq[6] == 0);
    } else CHECK(false);

    // ---- 7. fault memory of one module with full status, then clear all
    outText.clear(); CHECK(en.moduleFaults(bus, mi, false)); CHECK(has(outText, "status: test failed now") && has(outText, "stored: Frequency counter 3"));
    en.onOut = nullptr; en.actClearAll(bus); en.actScanAll(bus);
    faults = 0; for (auto &r : en.scan) faults += (int)r.faults.size();
    CHECK(faults == 0); CHECK(has(slurp(en.lastHtml), "No faults stored"));
    remove(en.lastHtml.c_str()); remove(en.lastTxt.c_str()); remove(tracePath.c_str());

    // ---- 8. live data: values reach the graph callback
    int points = 0; en.onLive = [&](const std::string &, const std::vector<pk::Value> &v) { for (auto &x : v) if (!std::isnan(x.num)) points++; };
    if (mi >= 0) { en.selectModule(mi); std::vector<int> sel;
        for (int i = 0; i < (int)en.entries.size() && sel.size() < 1; i++) { std::vector<uint8_t> rq; std::string e;
            en.pack.buildRequest(en.pack.mods[mi].svc[en.entries[i].main], {}, rq, e);
            if (rq.size() == 3 && rq[0] == 0x22 && rq[1] == 0x00 && rq[2] == 0xA0) sel.push_back(i); }
        en.runLive(bus, mi, sel, {}, 3); CHECK(points == 3); }

    // safety notes mention the right hazards
    CHECK(has(safetyNote('R', "PSM", "PSM", "Nacharbeitsentl\xC3\xBC" "ftung"), "BRAKE BLEEDING"));
    CHECK(has(safetyNote('R', "PASM", "PASM", "Fahrzeug ausregeln auf Niveau"), "SUSPENSION"));
    CHECK(has(safetyNote('W', "Kombiinstrument", "Instrument cluster", "Coding"), "WRITES"));

    // ---- 9. regression against a real recorded car session (Panamera 970; the VIN serial is zeroed in the
    //        fixture). Guards the on-car decode: genuine freeze-frame values are kept, and unset (all-0xFF)
    //        env fields read back as "not recorded" instead of being scaled into 16777215 km / 51 V.
    //        Skipped unless the G1 pack and the fixture are both present (the pack is not shipped).
    if (has(packPath, "G1") && std::ifstream("tests/fixtures/car_970_scan.ptrace").good()) {
        ReplayBus rb; CHECK(rb.load("tests/fixtures/car_970_scan.ptrace")); CHECK(rb.open());
        Engine e3; e3.quick = true; e3.reportDir = tmp; CHECK(e3.loadPack(packPath, err));
        e3.actScanAll(rb);
        std::string airEnv; bool airFound = false; const ScanRow *pdc = nullptr;
        for (auto &r : e3.scan) {
            if (r.req == 0x715) for (auto &f : r.faults) { if (f.d.code == 0x00440B) { airEnv = f.env; airFound = true; } }
            if (r.req == 0x70A) pdc = &r;
        }
        CHECK(airFound);
        CHECK(has(airEnv, "Mileage 266752 km")); CHECK(has(airEnv, "Voltage 14.4 V"));
        CHECK(has(airEnv, "Unlearning counter 255"));                    // real 1-byte counter, not a sentinel
        CHECK(pdc && !pdc->faults.empty());
        bool unset = false, nonsense = false;
        if (pdc) for (auto &f : pdc->faults) { if (has(f.env, "not recorded")) unset = true;
            if (has(f.env, "16777215") || has(f.env, "51 V")) nonsense = true; }
        CHECK(unset); CHECK(!nonsense);
        remove(e3.lastHtml.c_str()); remove(e3.lastTxt.c_str());
        printf("car-replay: real env values kept, unset fields -> not recorded\n");
    }

    printf(fails ? "%d FAILURE(S)\n" : "ALL ENGINE TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
