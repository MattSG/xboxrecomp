import hashlib
import json
from pathlib import Path

def capture_metadata(xbe, function, state, output):
    """Write hashes and state references; never copy XBE bytes into a case."""
    path = Path(xbe)
    result = {"format": 1, "xbe": {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()},
              "function": function, "state": str(Path(state)), "copyrighted_bytes": False}
    Path(output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
