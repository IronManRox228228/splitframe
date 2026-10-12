# SplitFrame — Strategic Hackathon Execution Plan (NEW_PLAN.md)

**Target Event:** Open Agent Hackathon 2026 (Oct 22–27, 2026 — 144 Hours)  
**Track:** Wildcard [Tinkerer] (Bring Your Own Project)  
**Primary Goal:** Deliver a top-tier, winning hackathon submission for SplitFrame that maximizes judging rubric points to fund code-signing certificates for Kerfd, while establishing SplitFrame as the reference open-source AI-native NLE.  
**Repository Target:** `IronManRox228228/splitframe` (`fork/main`). *Never touch upstream `rakesh0x/OpenCardboard`.*  
**Hard Constraints:** Warning-free `/WX` builds (MSVC 2022 / C++20 / Qt 6.8), zero unauthorized binary/model downloads, Kerfd code strictly proprietary and isolated.

---

## 1. Executive Summary & Opus' Core Findings

### 1.1 Independent Assessment Summary
SplitFrame possesses exceptional technical depth for an open-source project created by a solo 17-year-old engineer. It is not an LLM wrapper around a Python script or an ffmpeg CLI shell; it is an industrial-grade, native C++20/Qt 6.8 non-linear video editor with hardware-accelerated decode, professional color management, and a semantic timeline copilot.

However, **hackathons are won on visual demonstration, sponsor integration, and clear impact stories**, not hidden test suites. 

### 1.2 Verdict & Scorecard

| Dimension | Weight | Current Gut Score | Opus' Assessment & Action Required |
|---|---|---|---|
| **Impact** | **30%** | 🟢 Strong (with positioning) | Cardboard AI (YC W26) validated the market. SplitFrame opens it: *open-source, local-first, GPL-3.0 alternative to a funded SaaS*. Frame as democratizing professional post-production — "Linux to their Windows." |
| **Technical Depth** | **20%** | 🟢 Strong | Elite foundation: C++20, D3D11 zero-copy NV12/P010, OCIO 2.6 LUT baking, 54 unit tests across M1–M7 passing under `/WX`. |
| **Innovation** | **15%** | 🟡 Medium | Differentiate from generic "AI video generators" by highlighting **semantic edit understanding** (pacing, jump cuts, audio clipping, flash frames, branching timelines). |
| **Demo Video** | **15%** | 🔴 Weak (Current) | **Critical Risk:** A terminal running `ctest` printing `100% tests passed` scores zero with judges. We urgently need a **visual UI** showing real-time timeline linting, ghost edits, and branch comparison. |
| **Product & UX** | **10%** | 🔴 Weak (Current) | Must deliver a responsive, clean Qt 6.8 UI viewport with interactive timeline widgets. |
| **Sponsor Tech** | **10%** | 🟡 Unproven | Sponsor requirements are mandatory. Need concrete, functional integrations for **Meterless**, **Zetaris**, **NVIDIA**, and **Cursor**. |

### 1.3 Strategic Positioning: The Open Counterweight to YC W26

**Lineage:** SplitFrame is a direct fork of **OpenCardboard** (`rakesh0x/OpenCardboard`), which itself exists as the open-source alternative to **[Cardboard AI](https://www.cardboard.ai/)** — a YC W26-batch AI video editing startup.

This lineage is SplitFrame's single most powerful narrative asset for the hackathon. Here's why:

**The pitch isn't "we built a video editor." The pitch is:**

> *Cardboard AI proved the market — a YC W26 company charging for AI video editing as a service. SplitFrame is the open-source, local-first answer. Same AI-native editing intelligence. Zero cloud lock-in. Zero subscription. GPL-3.0. Your footage stays on your machine. Your GPU does the work.*

**Why this resonates with hackathon judges:**
1. **Validated market** — A YC-backed company already proved demand for AI video editing. SplitFrame doesn't need to argue the market exists.
2. **Democratization narrative** — Judges weight Impact at 30%. "Open-sourcing what a funded startup charges for" is the strongest possible democratization story.
3. **Technical differentiation** — Cardboard AI is cloud-first SaaS (browser-based, server-rendered). SplitFrame is native C++20/Qt 6.8, local-first, GPU-accelerated. Completely different architecture, not a clone.
4. **Philosophy differentiation** — Your data never leaves your machine. Inference routes through *your* chosen provider (Meterless), not a proprietary cloud. This is the "Linux to their Windows" narrative.

**Framing guardrails (what NOT to do):**
- ❌ Don't position *against* Cardboard AI in a hostile or competitive way. No trash-talking a YC company to judges.
- ❌ Don't imply SplitFrame copies Cardboard AI's code or features. It doesn't — entirely different stack.
- ✅ Frame as *"inspired by the vision, built on different principles."*
- ✅ Acknowledge Cardboard AI validated the market; SplitFrame opens it.

**Recommended pitch deck slide:**

> ### Why SplitFrame Exists
>
> AI video editing shouldn't be locked behind a subscription wall.
> Cardboard AI (YC W26) proved the market. SplitFrame opens it.
> Native C++20. Local-first. GPL-3.0. Your footage, your GPU, your edit.

---

## 2. Key Risks & Strategic Countermeasures

### Risk 1: The "Invisible Code" Demo Problem
* **The Hazard:** Judges review a 1–4 minute demo video. If the video only shows code or terminal output, SplitFrame will be eliminated regardless of its C++20 sophistication.
* **Countermeasure:** Pivot from headless engine work to **visual frontend surfacing**. Build a dedicated Qt 6.8 timeline view with visual problem badges, ghost suggestion overlays, and interactive branch diffing.

### Risk 2: "Bring Your Own Project" Bias
* **The Hazard:** In the Tinkerer track, judges scrutinize pre-existing codebases to ensure participants didn't just submit work finished months prior.
* **Countermeasure:** Clearly document the delta. M1–M7 establishes the native NLE foundation; the **144-hour hackathon delta** is:
  1. Real-time federated footage discovery via **Zetaris**.
  2. Dynamic inference & VRAM offloading via **Meterless**.
  3. Interactive Visual Copilot UI (ghost timeline cuts, visual lint markers, branch diffing).
  4. End-to-end autonomous assistant workflow.

### Risk 3: Sponsor Integration Friction
* **The Hazard:** Zetaris and Meterless APIs could have documentation gaps, SDK mismatches, or unexpected latency during the hackathon window.
* **Countermeasure:** Connectors must have deterministic fallbacks and clean interface isolation (`sf_agent::LlmClient` and `sf_index::CatalogProvider`) so that network blips during recording never break the live demo.

### Risk 4: Scope Trap (M8 Colour Scopes vs Hackathon Deliverables)
* **The Hazard:** Spending the 144 hours implementing full DaVinci-grade GPU Waveform/Vectorscope/Histogram compute shaders (M8) will burn time without addressing the core hackathon agent rubric.
* **Countermeasure:** **Freeze deep M8 shader development.** Keep existing OCIO 2.6 LUT baking and basic matrix grading; focus 100% of the 144-hour sprint on Agent UI, Sponsor APIs, and Demo Polish.

---

## 3. Mandatory Sponsor Integration Architecture

```
                                  ┌────────────────────────────────────────┐
                                  │           Cursor IDE                   │
                                  │   (Development & Agentic Scaffolding)  │
                                  └────────────────────────────────────────┘
                                                      │
                                                      ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                       SplitFrame Native NLE                                             │
│                                                                                                         │
│   ┌──────────────────────────┐    ┌──────────────────────────┐    ┌─────────────────────────────────┐   │
│   │   Zetaris Connector      │    │  Meterless Client        │    │  NVIDIA Hardware Engine         │   │
│   │   (sf_index)             │    │  (sf_agent::LlmClient)   │    │  (sf_render & sf_decode)        │   │
│   │                          │    │                          │    │                                 │   │
│   │ • Federated asset search │    │ • Specialist routing     │    │ • D3D11 zero-copy NV12/P010     │   │
│   │ • Cloud/Local B-roll     │    │ • VRAM JIT offload       │    │ • NVENC hardware encoding       │   │
│   │ • Cross-storage metadata │    │ • Token management       │    │ • Real-time 4K 60fps playback   │   │
│   └─────────────┬────────────┘    └─────────────┬────────────┘    └────────────────┬────────────────┘   │
│                 │                               │                                  │                    │
│                 └───────────────────────┐       │       ┌──────────────────────────┘                    │
│                                         ▼       ▼       ▼                                               │
│                               ┌──────────────────────────────────┐                                      │
│                               │      Visual Copilot Engine       │                                      │
│                               │  • Semantic Edit Linter          │                                      │
│                               │  • Ghost Suggestion Overlay      │                                      │
│                               │  • Branch Manager & Diff UI      │                                      │
│                               └──────────────────────────────────┘                                      │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Meterless (Inference & Resource-Aware Compute)
* **Role:** Primary cloud inference backend for SplitFrame's multi-agent copilot.
* **Key Innovation (VRAM Contention JIT Offload):** When SplitFrame is actively playing back 4K media or running high-bitrate NVENC exports, local GPU VRAM is strained. SplitFrame dynamically offloads all agent LLM queries (transcript analysis, cut suggestions, review) to Meterless.
* **Implementation:** Specialized HTTP client in `native/src/agent/meterless_client.h/cpp` implementing `LlmClient` interface with low-latency streaming and token telemetry.

### 3.2 Zetaris (Federated Data Virtualization)
* **Role:** Unified media intelligence across distributed storage pools.
* **Key Innovation (Federated Production Footage Catalog):** Video productions spread raw clips across local NVMe drives, networked studio NAS, and cloud buckets (S3/GCS). Zetaris enables the SplitFrame agent to query metadata, scene logs, and transcripts across all sources using unified SQL/metadata queries without prior local indexing.
* **Implementation:** `native/src/index/zetaris_provider.h/cpp` integrating with Zetaris query endpoints to fetch clip candidates for the editor copilot.

### 3.3 NVIDIA (Hardware Pipeline)
* **Role:** Core acceleration backbone.
* **Capabilities Highlighted:** Zero-copy D3D11 NV12/P010 video decoding (`sf_decode`), real-time GPU matrix/LUT transforms, and NVENC hardware video export pipeline (`sf_render`).

### 3.4 Cursor (Development Workflow)
* **Role:** Development IDE for the entire 144-hour sprint. Highlighted in submission documentation, prompt design files, and engineering logs.

---

## 4. Visual Copilot UI Sprint (Closing the Demo Gap)

To win 15% on Demo and 10% on Product & UX, the native Qt 6.8 interface must display the following features live:

### 4.1 Visual Timeline & Playback Viewport
* **Timeline Tracks:** High-performance Qt scene showing video clips, audio waveforms, and subtitle tracks.
* **Hardware Preview:** Native window viewport receiving D3D11 decoded frames at 60 fps.

### 4.2 Real-Time Semantic Lint Markers
* When the user makes an edit, `sf_copilot::LintEngine` evaluates the timeline.
* Detected issues appear as visual colored badges on the timeline:
  - 🔴 **Flash Frame:** Single-frame stray clip.
  - 🟠 **Jump Cut:** Sequential clips with matching camera angle & subject.
  - 🟡 **Audio Abrupt End:** Unfaded clip ending during speech.
  - 🔵 **Mid-Word Cut:** Cut intersecting a phoneme.
* Clicking any marker opens an explanation card with the proposed one-click fix.

### 4.3 Ghost Suggestion Overlays
* Proposed agent operations (e.g., trimming silences, inserting B-roll from Zetaris, pacing adjustments) render as semi-transparent **"ghost clips"** directly above or on the track.
* User actions: **[Accept (Tab)]**, **[Reject (Esc)]**, or **[Branch & Test]**.

### 4.4 One-Click Timeline Branching & Diff Viewer
* Visual selector to switch between `main` and `copilot/rough-cut-1`.
* Visual timeline diff view highlighting added clips (green), deleted ranges (red strikethrough), and shifted clips (amber).

---

## 5. 144-Hour Hackathon Execution Roadmap (Oct 22–27, 2026)

### Phase 0: Pre-Hackathon Staging (Oct 12 – Oct 21)
- [ ] Review Zetaris API documentation and developer portal access; build scratchpad test client in Python/C++.
- [ ] Test Meterless API endpoints and model routing capabilities.
- [ ] Prepare clean Qt 6.8 UI layout templates in `native/src/ui/`.
- [ ] Prepare sample multi-cam footage dataset (speech, B-roll, intentional bad cuts for linting).

---

### Phase 1: Days 1–2 (Oct 22–23) — Sponsor Connectors & Core Integration
*Goal: Working end-to-end pipeline connecting Meterless & Zetaris with zero warnings under `/WX`.*

- **Milestone H1: Meterless LLM Connector**
  - Implement `native/src/agent/meterless_client.h/cpp`.
  - Wire into `sf_agent::Router` and `sf_agent::Specialist`.
  - Implement VRAM-aware inference switching (Local vs Meterless Cloud).
  - Add unit test suite `native/tests/tst_meterless.cpp`.
- **Milestone H2: Zetaris Federated Media Provider**
  - Implement `native/src/index/zetaris_provider.h/cpp`.
  - Expose federated search API: `searchFootage(query, tags, speechText)`.
  - Connect to Copilot rough cut generator.
  - Add unit test suite `native/tests/tst_zetaris.cpp`.
- **Milestone H3: Build Verification**
  - Run full test suite (`ctest --preset debug`); ensure 56+/56 tests pass warning-free.

---

### Phase 2: Days 3–4 (Oct 24–25) — Visual Copilot UI Implementation
*Goal: High-impact, visually compelling Qt 6.8 interface for the live demo.*

- **Milestone H4: Timeline Lint Overlays**
  - Implement `TimelineLintOverlay` widget.
  - Subscribe to `sf_copilot::LintEngine` change events.
  - Render interactive badges at problem timecodes.
- **Milestone H5: Ghost Suggestions & Interactive Diff**
  - Implement `GhostClipItem` rendering on timeline tracks.
  - Implement branch toggle switch in the UI header.
  - Add side-by-side or layered diff visualization for alternative cuts.
- **Milestone H6: End-to-End User Flow Integration**
  - Connect Zetaris search bar in media bin.
  - Trigger copilot edit plan with live progress indicators.

---

### Phase 3: Day 5 (Oct 26) — Demo Production & Pitch Assets
*Goal: Flawless 1–4 minute demo video, compelling slide deck, and comprehensive documentation.*

- **Milestone H7: Video Production (The Winning Demo)**
  - Follow the scripted 3-minute storyboard (see Section 6).
  - Record high-framerate 4K footage of SplitFrame running smoothly on the RTX 4060.
  - Edit pacing tight: hook → problem → sponsor-powered agent action → magic result.
- **Milestone H8: Slide Deck & Documentation**
  - 10-slide deck emphasizing Impact (democratizing indie post-production), Architecture, Sponsor Integrations, and Future Roadmap.
  - Overhaul project README with animated GIFs, architecture diagrams, and clear build instructions.
  - Complete Sponsor Technology Write-up according to hackathon guidelines.

---

### Phase 4: Day 6 (Oct 27) — Verification & Final Submission
*Goal: Stress-test submission package and submit well before deadline.*

- [ ] Audit against all 6 judging criteria (Impact, Tech, Innovation, Demo, Product/UX, Sponsors).
- [ ] Verify clean clone & build reproducibility.
- [ ] Confirm submission form entries, video link, and public GitHub repository state (`fork/main`).
- [ ] Submit before final deadline.

---

## 6. The 3-Minute Demo Video Storyboard

| Timestamp | Scene | Audio / Voiceover Narrative | On-Screen Action |
|---|---|---|---|
| **0:00 – 0:35** | **The Hook: Why SplitFrame Exists** | *"AI video editing is a proven market — YC W26 is backing it as a cloud service. But professional post-production shouldn't be locked behind a subscription wall. SplitFrame is the open-source, local-first alternative. Native C++20, GPU-accelerated, GPL-3.0. Your footage never leaves your machine."* | Quick montage: solo creator drowning in raw clips. Title card: "SplitFrame — Open-Source AI-Native Video Editing." |
| **0:35 – 1:15** | **Zetaris: Federated Asset Intelligence** | *"Instead of hours searching local drives and cloud buckets, SplitFrame queries Zetaris. With one natural language request, the agent federates footage across all storage pools and retrieves the exact B-roll reaction shot."* | Agent search prompt: "Find me the wide angle of the discussion." Zetaris immediately fetches metadata and drops clip candidate into bin. |
| **1:15 – 1:55** | **Meterless: Low-Latency Semantic Copilot** | *"Under the hood, Meterless powers our specialist LLM routing. While our D3D11 pipeline renders 4K NV12 playback without dropping a frame, Meterless handles the edit intelligence without fighting for local VRAM."* | Showing smooth 60fps playback while the Copilot streams timeline analysis in real time. |
| **1:55 – 2:35** | **The Magic: Edit Linter, Ghosts & Branching** | *"SplitFrame doesn't just cut—it understands grammar. Notice the red lint badges: jump cut detected, audio clipped abruptly. The copilot renders a ghost suggestion. With one keystroke [Tab], it's accepted. Want to try another pacing style? Branch the timeline and compare diffs instantly."* | Bad cuts flagged with badges; user presses Tab to accept ghost fix; user switches branches to show alternative edit version instantly. |
| **2:35 – 3:00** | **Conclusion & Call to Action** | *"The market for AI video editing is validated. SplitFrame opens it. Native C++20, Qt 6.8, NVIDIA NVENC, powered by Zetaris and Meterless. GPL-3.0 — free forever. Pro studio intelligence for every creator."* | Lightning-fast NVENC export completes. Final clean cut plays. SplitFrame logo, GitHub link, "Star us — fork us — ship with us." |

---

## 7. Submission Checklist & Rubric Verification

- [ ] **Track:** Wildcard [Tinkerer] clearly selected.
- [ ] **Remote:** Committed and pushed strictly to `fork/main` (`IronManRox228228/splitframe`).
- [ ] **Zero Warnings:** MSVC 2022 `/WX` compile succeeds cleanly.
- [ ] **Unit Tests:** All unit test suites pass (`ctest --preset debug`).
- [ ] **Sponsor Verification:**
  - Meterless: Working API client code and VRAM JIT offload demo.
  - Zetaris: Federated catalog provider code and demonstration.
  - NVIDIA: Documented NVENC export and D3D11 decode architecture.
  - Cursor: Documented development usage.
- [ ] **Assets:**
  - 1–4 minute MP4/YouTube demo video.
  - PDF / Google Slides pitch deck.
  - Public repository README with architecture flow and sponsor tech breakdown.
- [ ] **Proprietary Safeguard:** Ensure no files or code references to Kerfd (`Documents/SupaBrain`) exist anywhere in the repository.
