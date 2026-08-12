# Learning from exemplary technical whitepapers

Reference PDFs downloaded in this folder (canonical, widely-praised for writing quality):

| File | Paper | Why it's a good model |
|------|-------|-----------------------|
| `raft-consensus.pdf` | *In Search of an Understandable Consensus Algorithm* (Ongaro & Ousterhout, 2014) | Gold standard for **clarity**. Explicitly optimizes for the reader's understanding. Great use of diagrams + "condensed summary" boxes. |
| `amazon-dynamo.pdf` | *Dynamo: Amazon's Highly Available Key-value Store* (DeCandia et al., 2007) | Best **structure** to copy: Intro → Background → Related Work → Architecture → Implementation → **Experiences & Lessons** → Conclusion. |
| `google-mapreduce.pdf` | *MapReduce* (Dean & Ghemawat, 2004) | Masterclass in **motivation-first** writing + a killer worked example early. Concise (13pp). |
| `google-bigtable.pdf` | *Bigtable* (Chang et al., 2006) | Clean **data-model exposition** before implementation; strong "Real Applications" (use-cases) section. |
| `google-spanner.pdf` | *Spanner* (Corbett et al., 2012) | Excellent **evaluation** section — how to present benchmark tables and defend numbers. |

---

## What these papers do that a great whitepaper needs

### 1. Structure (the reliable skeleton)
Dynamo's outline is the canonical template:
1. **Introduction** — the problem, in the reader's terms, and a one-paragraph "here's what we built + headline result."
2. **Background / Motivation** — why existing tools fall short (be specific, name them).
3. **Related work** — position yourself honestly against alternatives.
4. **Design / Architecture** — the core contribution. One idea per subsection.
5. **Implementation** — the parts that were actually hard.
6. **Evaluation / Benchmarks** — numbers, methodology, honest caveats.
7. **Experiences & Lessons Learned** — the section readers remember most. Underused.
8. **Conclusion** — restate contribution + one forward-looking sentence.

### 2. The abstract does real work
All five: 4–6 sentences = problem, approach, one concrete headline number, what's novel. No fluff, no "in this paper we will."

### 3. Motivation before mechanism
MapReduce shows a *worked example* (word count) before any architecture. Lesson: lead each section with **why**, then **how**.

### 4. Evaluation is a first-class citizen
Spanner/Dynamo: every claim is measured. They (a) describe the setup precisely (hardware, dataset, repetitions), (b) show tables/plots, (c) **discuss the caveats themselves** so reviewers trust them. State what varies and what's stable.

### 5. Honesty builds authority
Dynamo's "Experiences & Lessons Learned" and explicit trade-off discussions make it credible. Admitting a limitation is more persuasive than hiding it.

### 6. Typography & layout (what makes them look serious)
- Tight, consistent section hierarchy; generous whitespace; no decorative clutter.
- Every table has a caption that states the *takeaway*, not just "Results."
- Figures are referenced in prose ("as Figure 3 shows...") — never floating alone.
- One idea per paragraph; short topic sentences.

---

## Applying this to the pigzpp whitepaper

Current structure: Abstract → Intro → Design → Benchmarks → Use cases → Packaging → Reproducibility → Credits → References. Already strong. Highest-value upgrades, in order:

- [ ] **Add a short "Related Work / Why not X" subsection** — contrast pigzpp vs pigz, zlib-ng CLI, libdeflate, zstd. One honest paragraph. (Dynamo §3)
- [ ] **Add a "Lessons Learned" paragraph** near the end — what the AI-assisted rewrite taught you, what was hard (global state removal, thread-safety, ABI). This is the memorable section and fits your "AI-assisted modernization experiment" angle. (Dynamo §6)
- [ ] **Lead the Design section with one worked example** — trace a single 128 MB file through the parallel pipeline (split → per-block deflate → CRC combine → concat). (MapReduce technique)
- [ ] **Benchmark captions state the takeaway** — e.g. "pigzpp sustains 7× pigz throughput at equal ratio" rather than "CLI benchmark results."
- [ ] **Add explicit caveats sentence** to each benchmark table (dataset, cores, best-of-N) — you already have the global note; make per-table deltas explicit where relevant. (Spanner discipline)
- [ ] **Reference every table/figure in prose** before it appears.
