/* module.json through Schwung's OWN page planner and contract validator (the
 * code the device runs): page 1 = MIX, page 2 = TONE (MIX shared on knob 1),
 * page 3 = INFO (read-only), labels fit, values format.
 *
 *   node tests/test_ui.mjs <schwung-root> <project-root>
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiro Ikezawa
 */
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const [schwung, project] = process.argv.slice(2);
const load = (rel) => import(pathToFileURL(path.join(schwung, rel)).href);
const { planPages } = await load("src/shared/param_pages/page_plan.mjs");
const { validateContract } = await load("src/shared/param_pages/validate_contract.mjs");
const { labelForCell } = await load("src/shared/param_pages/render_page_movy.mjs");
const { formatParamValue } = await load("src/shared/param_format.mjs");

let failures = 0;
const check = (cond, msg) => { console.log((cond ? "  ok " : "FAIL ") + msg); if (!cond) ++failures; };

const m = JSON.parse(fs.readFileSync(path.join(project, "module.json"), "utf8"));
const caps = m.capabilities;
check(m.component_type === "audio_fx" && caps.chainable && caps.audio_in && caps.audio_out, "chainable audio_fx");
check(m.dsp === `${m.id}.so`, "dsp is <id>.so (the chain host loads audio_fx/<id>/<id>.so)");
check(caps.requires_continuous_processing === true, "requires_continuous_processing (a silent slot must not park the input)");

const res = planPages({ hierarchy: caps.ui_hierarchy, chainParams: caps.chain_params });
const pages = res.pages || res;
const knobPages = pages.filter((pg) => (pg.keys || []).length);
const want = [
    ["mix", "an_level", "usb_level", "source", "an_gain", "usb_gain", "duck", "mute"],
    ["mix", "an_hpf", "usb_hpf", "an_lpf", "usb_lpf", "an_pan", "usb_pan", "width"],
    ["status", "in_level"],
];
check(knobPages.length === 3, `three knob pages (got ${knobPages.length}: ${knobPages.map((p) => p.title || p.name).join(", ")})`);
want.forEach((w, i) => {
    const got = knobPages[i] ? knobPages[i].keys : [];
    check(JSON.stringify(got) === JSON.stringify(w), `page ${i + 1} (${knobPages[i] && (knobPages[i].title || knobPages[i].name)}) knobs = ${got.join(" ")}`);
});

const { findings } = validateContract({ id: m.id, hierarchy: caps.ui_hierarchy, chainParams: caps.chain_params });
for (const f of findings) check(f.level === "info", `[${f.level}] ${f.message}`);
if (!findings.length) check(true, "contract validator: no findings");
for (const c of caps.chain_params) {
    const l = c.short_name || labelForCell(c.name);
    check(l.length > 0 && l.length <= 6, `label ${c.key} -> ${l}`);
}
const meta = (k) => caps.chain_params.find((c) => c.key === k);
const fmt = (k, v) => formatParamValue(v, meta(k));
for (const [k, v] of [["mix", 1], ["an_level", 0.8], ["source", 1], ["an_gain", -6], ["duck", 0.5], ["mute", 1],
                      ["an_hpf", 20], ["an_hpf", 1000], ["an_lpf", 20000], ["usb_lpf", 2500], ["an_pan", -100],
                      ["width", 200], ["status", 2], ["in_level", -12.3]])
    console.log(`  ${k.padEnd(9)} ${String(v).padStart(5)} shows ${fmt(k, v)}`);
check(fmt("mix", 1) === "100%", "Mix 1 shows 100%");
check(["status", "in_level"].every((k) => meta(k).access === "read" && meta(k).live === true), "readouts are read-only + live");
check(JSON.stringify(meta("source").options) === '["ANALOG","USB-C"]', "SOURCE = ANALOG / USB-C (no BOTH: one input stream, see README)");
console.log(failures ? `FAILED: ${failures}` : "PASS");
process.exit(failures ? 1 : 0);
