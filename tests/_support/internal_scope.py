"""OptiScaler/State.h's ScopedInternalResourceCreation, as written, for host fixtures.

The scope is how OptiScaler marks what it creates through the game's device as its own, for the heuristics that watch
that device (the DLSS-NR exposure scan). A fixture that compiles production code using it compiles the production
class rather than a stand-in: the class is its whole contract (a counter on the creating thread, no lock), and it
names nothing outside the language, so it needs no fake.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = "ScopedInternalResourceCreation"

# The declaration every site uses, so a source guard can find where a scope opens.
DECLARATION = NAME + " internalResources {};"


def scope_class():
    """The class definition from State.h, ending at its closing brace."""
    text = (ROOT / "OptiScaler/State.h").read_text(encoding="utf-8-sig")
    opening = "class " + NAME + "\n{"
    assert text.count(opening) == 1, f"expected one definition of {NAME} in OptiScaler/State.h"
    start = text.index(opening)
    return text[start:text.index("\n};", start) + len("\n};")] + "\n"
