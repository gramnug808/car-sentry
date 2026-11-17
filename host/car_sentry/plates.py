"""Hawaii plate OCR repair — same rules as newtool v3.2/v3.3.

Patterns (first-class, equal):
  * standard  ABC123  — 3 letters + 3 digits
  * reversed  123ABC  — 3 digits + 3 letters
  * custom    e.g. KEVIN1, 808KOA, ALOHA (mixed, or all-letter)

Positional confusion repair (only in the slot where it applies):
  letter slot:  0->O, 1->I, 5->S, 8->B, 2->Z
  digit slot:   O->0, I->1, S->5, B->8, Z->2
  NOTE: a reversed letter-slot '8' repairs to 'B', NOT 'C'
  (123A8C -> 123ABC, 123AB8 -> 123ABB). OCR misreads B as 8;
  there is no 8->C repair anywhere.

Split tokens are joined before repair: "ABC 123" -> "ABC123",
"A B C 1 2 3" -> "ABC123".

Scoring: a structured plate (ABC123 / 123ABC) always outranks a
custom plate. All-letter custom (ALOHA) scores lower than a mixed
custom (KEVIN1). Each positional repair costs points, so an exact
read beats a repaired read.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

LETTER_SLOT = {"0": "O", "1": "I", "5": "S", "8": "B", "2": "Z"}
DIGIT_SLOT = {"O": "0", "I": "1", "S": "5", "B": "8", "Z": "2"}

STANDARD = "L" * 3 + "D" * 3   # ABC123
REVERSED = "D" * 3 + "L" * 3   # 123ABC


@dataclass
class PlateResult:
    plate: str
    kind: str          # "standard" | "reversed" | "custom"
    score: int         # 0-100, higher is more confident
    repairs: int
    raw: str

    def as_dict(self):
        return {"plate": self.plate, "kind": self.kind, "score": self.score,
                "repairs": self.repairs, "raw": self.raw}


def clean_raw(raw: str) -> str:
    """Upper-case, drop every non-alphanumeric (joins split tokens)."""
    return re.sub(r"[^A-Z0-9]", "", (raw or "").upper())


def _apply_pattern(cleaned: str, pattern: str):
    """Try one L/D pattern. Returns (plate, repairs) or None if unrepairable."""
    if len(cleaned) != len(pattern):
        return None
    out, repairs = [], 0
    for ch, slot in zip(cleaned, pattern):
        if slot == "L":
            if ch.isalpha():
                out.append(ch)
            elif ch in LETTER_SLOT:
                out.append(LETTER_SLOT[ch])
                repairs += 1
            else:
                return None
        else:  # digit slot
            if ch.isdigit():
                out.append(ch)
            elif ch in DIGIT_SLOT:
                out.append(DIGIT_SLOT[ch])
                repairs += 1
            else:
                return None
    return "".join(out), repairs


def candidates(raw: str) -> list[PlateResult]:
    """All plausible interpretations of raw OCR text, best first."""
    cleaned = clean_raw(raw)
    results: list[PlateResult] = []
    if not cleaned:
        return results

    if len(cleaned) == 6:
        for pattern, kind in ((STANDARD, "standard"), (REVERSED, "reversed")):
            hit = _apply_pattern(cleaned, pattern)
            if hit:
                plate, repairs = hit
                results.append(PlateResult(plate, kind, 100 - 5 * repairs,
                                          repairs, raw))
    # Custom plates: 2-7 chars, at least one letter. Never outrank a
    # structured plate of the same raw text (structured scores >= 70
    # after repairs; custom caps at 65). All-letter scores lowest.
    if 2 <= len(cleaned) <= 7 and any(c.isalpha() for c in cleaned):
        has_digit = any(c.isdigit() for c in cleaned)
        score = 65 if has_digit else 50
        # Only add if it is not already a structured hit with same text.
        if not any(r.plate == cleaned for r in results):
            results.append(PlateResult(cleaned, "custom", score, 0, raw))

    results.sort(key=lambda r: (-r.score, r.repairs))
    return results


def repair_plate(raw: str) -> PlateResult | None:
    """Best interpretation of raw OCR text, or None if nothing plausible."""
    cands = candidates(raw)
    return cands[0] if cands else None


def is_structured(plate: str) -> bool:
    """True for ABC123 or 123ABC shape (after repair)."""
    return bool(re.fullmatch(r"[A-Z]{3}[0-9]{3}|[0-9]{3}[A-Z]{3}", plate or ""))
