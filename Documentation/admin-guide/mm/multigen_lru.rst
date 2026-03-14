.. SPDX-License-Identifier: GPL-2.0

===================
Multi-Gen LRU Lite
===================

This tree carries a lightweight, KMI-safe Multi-Gen LRU implementation.
It is designed for Android kernel bring-up and iterative tuning, while
avoiding KMI-visible layout changes to core MM structs.

Compared with upstream full MGLRU, this variant keeps the reclaim path
conservative and incremental.

Build-time options
==================
Enable the feature via:

* ``CONFIG_LRU_GEN=y``

Optional knobs:

* ``CONFIG_LRU_GEN_DEBUG_LOG``
  Enable periodic debug logs (recommended off for production).
* ``CONFIG_LRU_GEN_LITE_ACCESS_CTRL``
  Enable lightweight access-bit based promotion from old generations.
* ``CONFIG_LRU_GEN_LITE_PTWALK``
  Enable bounded page-table-walk style sampling via ``page_referenced()``.

Design summary
==============

Implemented
-----------

* Side-table state per ``lruvec`` (hash table), no KMI-visible MM struct changes.
* Generation queues for anon/file pages, reclaim from oldest generation.
* Tiering for file pages:
  Tier-0 cold, Tier-1 warm (``PageReferenced``), Tier-2 hot (``PageWorkingset``).
* Aging with bounded generation window and conservative fallback to classic LRU.
* Refault feedback to produce temporary boost levels.
* Lite look-around feedback accounting.
* Thrashing-oriented protection via tier budgets and boost windows.
* PTWALK sampling feedback:
  sampled hot ratio is used as a suppressor for boost and budget aggressiveness.

Not implemented (vs upstream full MGLRU)
----------------------------------------

* Full memcg/mm list walk based aging pipeline.
* Full page-table accessed-bit harvesting across mm address spaces.
* Upstream ``/sys/kernel/mm/lru_gen/*`` runtime ABI and debug command set.

Runtime behavior notes
======================

* This implementation is compile-time controlled; there is no runtime kill-switch
  ABI equivalent to upstream ``lru_gen/enabled``.
* Under pressure, refault spikes can temporarily raise reclaim protection, but
  PTWALK hot-ratio feedback can clamp boost level and shrink tier budgets.
* In extreme synthetic stress tests (RAM+ZRAM saturation), higher boost levels
  may still appear; tune for device UX with mixed workloads, not only worst-case
  saturation tests.

Debug logging
=============

When ``CONFIG_LRU_GEN_DEBUG_LOG`` is enabled, periodic ``mglru-lite:`` logs expose
counters such as:

* ``age``, ``add``, ``del``, ``scan_pick``, ``scan_fallback``
* ``tier_add``, ``tier_protect_skip``, ``tier_budget``
* ``boost_level``, ``refault_boost``
* ``look_around`` (local/global)
* ``access_promote``
* ``ptwalk`` hot/total and ``ptwalk_clamp``

These fields are intended for bring-up and tuning only.

Tuning guidance
===============

* Keep debug logging disabled on release builds.
* Evaluate with both:
  * daily app-switch / standby scenarios (UX-oriented), and
  * synthetic stress (boundary behavior).
* If background retention regresses, first reduce sustained high boost windows
  before increasing tier protection budgets.
