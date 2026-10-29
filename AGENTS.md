# AGENTS.md

Project overview and license: [README.md](README.md). The scripts in `tools/` and what each one is for: [tools/README.md](tools/README.md).

## Rules

- Comments and output messages in code, scripts and config files are in English, as is this file. `README.md` is in English and `README_CN.md` in Chinese; keep the two in sync, each with a language switch at the top. Other docs are in Chinese.
- The emulation core, interpreter, JIT and device models are written from scratch; do not adopt or port code from existing emulators. Third-party test ROMs, test cases and their expected results may be used as references.

## Build

- The firmware builds with ESP-IDF v6.1, installed with EIM. The IDF environment is not set globally, because activating it replaces `python` and `cmake` with IDF's own. Run IDF commands through EIM:
  - Windows, PowerShell: `eim run 'idf.py --preset jc1060p470-debug build; exit $LASTEXITCODE' v6.1`. Without `exit $LASTEXITCODE`, `eim run` exits with 0 even when the command fails.
  - macOS, zsh: `eim run 'idf.py --preset jc1060p470-debug build' v6.1`.
- Building the main firmware needs [uv](https://docs.astral.sh/uv/): the build runs `tools/ui/pack_assets.py` to pack the board's UI assets for the assets partition.
- Presets in `CMakePresets.json` select the board, the build type and firmware or test firmware. Each preset builds in `build/<preset>/` and keeps its generated `sdkconfig` there; it is not committed. Settings to keep go into `sdkconfig.defaults`, `sdkconfig.defaults.debug` or `.release`, `sdkconfig.defaults.profile` for the profiling presets, or `boards/<board>/sdkconfig.defaults`.
- Always pass `--preset`. Run on a build directory without it, `idf.py` applies the first preset in `CMakePresets.json`, `jc1060p470-debug`, whose cache variables replace the directory's board and build type. Build a variant through a preset or board defaults, not through a hand-edited `sdkconfig` in a directory of its own.
- The PC build is the CMake project in `host/`: run `cmake --preset debug` and `cmake --build --preset debug` there.
- ESP32-S31 is a preview target in v6.1, so selecting it needs `idf.py --preview set-target esp32s31`, run through `eim run` as above. Once the target is set, `idf.py build` works without the flag. ESP32-P4 needs no flag.
- The `<board>-test-profile` presets build the test firmware with `GBX_PROFILER`: a sampling profiler and JIT counters, used through `tools/profile_report.py`. The `<board>-profile` presets build the release firmware with `GBX_PROFILER`: profiling commands on the serial console, see `docs/debug-design.md`. Build them only for performance analysis. They are not part of regular builds, tests or acceptance, and their timings include the profiler's overhead. Profiling code stays inside `#ifdef GBX_PROFILER`, so other builds contain none of it.

## Code

- The layout, module boundaries, build and test setup are described in [`docs/architecture.md`](docs/architecture.md).
- gnu++23, with `-fno-exceptions -fno-rtti`, on the firmware and the PC build. `core`, `jit` and `app` under `components/` include only the platform interface in `components/platform/include/gbx/`, never ESP-IDF headers.
- Paths that run per instruction or per cycle allocate no heap memory and make no virtual calls. Measure code size before specializing hot code with templates.
- Control interpreter, device and JIT runtime hot-code placement in the build/linker configuration so unrelated application, test or JIT compiler changes do not move it. Keep runtime helpers and block lookup separate from compilation and cache management code. Verify addresses in the link map and measure cache-sensitive layout changes on the board; alignment alone is not evidence of an improvement. Keep the layout rules shared by firmware and test builds.
- Name board directories after the board's model in lowercase, such as `boards/jc1060p470`.

## Commits

- Do not run git commit unless the user explicitly asks for a commit.
- Make each commit one logical change.
- Write commit messages in English. The subject line is at most 50 characters, in the imperative mood ("Add MBC1 banking", not "Added MBC1 banking"), capitalized and without a trailing period. When the change needs explaining, leave a blank line and write a body wrapped at 72 characters that says what changed and why.
- Do not add `Co-Authored-By` or any other attribution trailer.

## Tests

- The on-device test plan, SD card layout and test selection are described in [`docs/hardware-testing.md`](docs/hardware-testing.md).
- Flash only the firmware required by the next operation. Do not automatically restore the main firmware after tests; leave the current firmware in place until another operation needs a different build.
- Write only tests that can catch meaningful functional errors. Do not write defensive tests, or negative tests merely because something was removed.
- Test against the hardware's behavior, not the current implementation: CPU instructions with the SingleStepTests cases in `refs/tests/sm83` and `refs/tests/arm7tdmi`, the whole machine with the test ROMs listed in `refs/README.md`, each judged by its suite's own pass criteria, such as serial output, the registers at the final `LD B,B`, `r12` at the end of a GBA test, on-screen PASS/FAIL text, or the reference images.
- Compare frames only with the reference images a test suite ships, in the shades or colors it specifies. A reference that is a photo or a resized screenshot, such as those in `refs/tests/nba-hw-test`, is not an exact frame and is only for manual checks. Do not record the emulator's own output as a golden image or trace.
- GBA frames may also be compared with the reference frames in `tests/gba/frames/`, generated by `tools/gba_reference_frames.py` on the pinned reference emulator (NanoBoyAdvance v1.8.3) and checked as its description says: frames of the GBX PPU scenes must equal what `tests/gba/roms/ppu/model.py` computes from GBATEK, and a third-party frame is used only after the user has confirmed it against the suite's own description or photo. When the emulator and a reference frame disagree, GBATEK and hardware tests decide which one is right; do not change the emulator only to match the reference emulator. Other systems compare only with the images their suites ship.
- Every test that runs on the interpreter also runs on the JIT, and both must give the same result.
- Tests run on the board connected to the PC: test data stays on the SD card, and the runner on the PC reads the suites from `refs/tests`, selects cases and sends the execution list. SM83 normally uses on-device judgment with failure details; diagnostic mode returns lossless actual-state deltas for independent PC judgment. Both use the suite's criteria. The PC build has only the interpreter and is for development and debugging. Test suites stay out of the repository and are not redistributed: the original suites live in `refs/`, and the prepared card data in `tests/sdcard/`, a git-ignored copy of the card's `tests/` that the prepare scripts write and the runners read. Commercial game ROMs are never committed, and no test requires one.
- Name test cases in English.

## Writing

These rules cover Chinese prose: `README_CN.md`, the docs under `docs/` and other Chinese text. Code comments and program output are in English and fall outside them. Chinese prose should read as if a person wrote it. Machine prose gives itself away by trying too hard: it compresses, balances and ornaments, and the reader keeps stopping to decode it. Banning a pattern only brings the habit back in another shape, so correct the urge to sound crafted, not the surface form. The rules are cues for review, not a list of banned words; a paragraph that is clear and reads well needs no change. The core rules follow. Before writing or editing Chinese prose, also read [`docs/writing.md`](docs/writing.md): it holds the remaining rules, examples of the core ones, and the rules for editing existing text.

Facts and certainty come before every other rule:

- Write only what is known. Report performance, compatibility and test results as measured, with the platform and version; if something has not been measured, write “未实测”.
- Do not raise certainty. “可能” does not become “会”, a planned feature is not a finished one, two things happening together are not cause and effect, and “支持 MBC5” is not “MBC5 已通过测试”.
- When information is missing, say what is missing. Do not fill the gap with a guess, and do not keep repeating that sources are limited.
- Cite the source of hardware behavior, such as a Pan Docs section or a test ROM, rather than a vague “业界普遍认为” or “据说”.

Then:

- Do not balance clauses. Matched halves, semicolon couplets and an even beat across a paragraph are the strongest mark of machine prose, and they slip through because to the writer they feel like good rhythm. Let sentence length follow the meaning.
- Spell out the logic. When one clause turns against, causes or concedes to another, write the connective, such as “但是”“因为”“虽然”; do not set them side by side for the reader to infer.
- Keep the small grammatical words natural Chinese carries: aspect particles such as “了”“着”“过”, modal verbs, adverbs, result complements. Stripping them makes prose terse and telegraphic. Drop the small words that only add a flourish of contrast or restraint.
- Prefer common two-syllable words such as “已经” and “如果” to clipped single-syllable or classical ones such as “已” and “若”.
- Get to the point and say a thing once. Write no preview sentences such as “下面我们来看看”, no empty background such as “随着技术的不断发展”, and no closing platitudes such as “总之” or “值得期待”. Do not follow a point with a line explaining it, and do not open a section by restating its heading.
- Do not inflate. Cut words that carry no information, such as “标志着”“彰显了”“至关重要”“深入探讨”“无缝”“赋能”, and trailing verdicts such as “体现了……”; such a word may stay where it has its exact meaning, as “闭环” does in a control system. Do not stage contrasts with “不是 X，而是 Y” unless the first half corrects a misreading a reader might really have.
- Write hardware and emulation in the terms developers use. Keep register names, mnemonics and established terms such as PPU, M-cycle and MBC in their original form; do not coin Chinese paraphrases.
- Design docs and operation guides describe the current design, constraints and usage. Do not put test results, performance measurements or build-pass records in them. `docs/hardware-testing.md` is a testing guide; keep results and measurements in reports, and task completion in `docs/plans/`. Board docs retain hardware sources, identification and electrical/init constraints, not probe or validation reports. Do not write “这里新补充了……” or “现在改为……”, or leave notes about moved content. Record changes in commit messages or a changelog.
- In Simplified Chinese, use “” for quotes, names and section references; 「」 belongs to Traditional Chinese. Use full-width punctuation in Chinese text, and leave code, paths and commands as they are.

## References

Reference layout, download instructions and board-material conventions are documented in [refs/README.md](refs/README.md). When adding a downloaded reference, pin it to the same commit or release in both fetch scripts and list it in that index. Limit board materials to the files development needs and summarize them in `docs/boards/<board>.md`.
