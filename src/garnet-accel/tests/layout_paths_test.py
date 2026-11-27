#!/usr/bin/env python3
"""Test layout-directory plumbing and gate C orchestration without Apple binaries."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


def run_case(gates: Path, fixture: Path, code: str) -> None:
    env = dict(os.environ, GARNET_KC_WORK=str(fixture), GARNET_LAYOUT_OUT=str(fixture / "output"))
    env["PYTHONPATH"] = str(gates)
    subprocess.run([sys.executable, "-c", code, str(fixture)], env=env, check=True)


def main() -> None:
    gates = Path(__file__).resolve().parents[1] / "gates"
    if len(sys.argv) > 1:
        gates = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="garnet-layout-paths-") as tmp:
        fixture = Path(tmp)
        output = fixture / "output"
        output.mkdir()
        (output / "model.json").write_text('{"classes": {"fixture": 1}}')
        (output / "forwarded.json").write_text('{"fixture": [3]}')
        (output / "slots.json").write_text('{"classes": {"fixture": 2}}')
        run_case(gates, fixture, """
import common, os
from pathlib import Path
assert Path(common.LAYOUT).resolve() == Path(common.HERE).parents[2] / 'tools' / 'ioaccel-layout'
assert Path(common.LAYOUT, 'gen_header.py').is_file()
assert common.LAYOUT_OUT == os.environ['GARNET_LAYOUT_OUT']
assert common.model() == {'fixture': 1}
assert common.forwarded() == {'fixture': [3]}
import gen_tramp
assert gen_tramp.slots_json() == {'fixture': 2}
print('PASS: actual layout scripts and custom model/forwarded/slots output')
""")
        # These fixtures exercise orchestration only; they do not model an Apple ABI.
        generator = fixture / "gen_header.py"
        generator.write_text("""
import os
from pathlib import Path
p = Path(os.environ['GARNET_LAYOUT_OUT'])
assert p.name != 'output', 'regeneration must not overwrite the live layout'
p.joinpath('IOAccelFamily2_decl.h').write_bytes(p.joinpath('model.json').read_bytes())
""")
        (fixture / "check_layout.py").write_text("""
import os, sys
from pathlib import Path
p = Path(os.environ['GARNET_LAYOUT_OUT'])
assert p.name == 'output', 'layout check must use the selected live layout'
bad = p.joinpath('mismatch').exists()
print('MISMATCH fixture' if bad else 'MATCH fixture')
sys.exit(1 if bad else 0)
""")
        (output / "IOAccelFamily2_decl.h").write_bytes((output / "model.json").read_bytes())
        prelude = """
import gate_link, sys
from pathlib import Path
fixture = Path(sys.argv[1])
gate_link.LAYOUT = str(fixture)
output = fixture / 'output'
before = {p.name: p.read_bytes() for p in output.iterdir() if p.is_file()}
"""
        unchanged = """
assert before == {p.name: p.read_bytes() for p in output.iterdir() if p.is_file()}
"""
        run_case(gates, fixture, prelude + "assert gate_link.gate_c() == []\n" + unchanged
                 + "print('PASS: fresh header regenerated separately; live layout untouched')")
        (output / "IOAccelFamily2_decl.h").write_text("stale")
        run_case(gates, fixture, prelude + """
problems = gate_link.gate_c()
assert len(problems) == 1 and 'STALE' in problems[0], problems
""" + unchanged + "print('PASS: stale header rejected without repair in place')")
        (output / "IOAccelFamily2_decl.h").write_bytes((output / "model.json").read_bytes())
        (output / "mismatch").touch()
        run_case(gates, fixture, prelude + """
problems = gate_link.gate_c()
assert len(problems) == 1 and 'did not MATCH' in problems[0], problems
""" + unchanged + "print('PASS: layout mismatch remains a failure')")
        (output / "mismatch").unlink()
        generator.write_text("raise SystemExit('fixture generation failure')\n")
        run_case(gates, fixture, prelude + """
problems = gate_link.gate_c()
assert len(problems) == 1 and 'gen_header.py failed' in problems[0], problems
""" + unchanged + "print('PASS: generator failure remains a failure')")
    print("layout paths: 5 scenarios passed (no target-OS ABI validation)")


if __name__ == "__main__":
    main()
