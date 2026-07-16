from pathlib import Path
p = Path(__file__).resolve().parent.parent / "bridgesessions.cpp"
lines = p.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
out = []
i = 0
while i < len(lines):
    if i + 1 < len(lines) and "line.back() ==" in lines[i] and lines[i + 1].lstrip().startswith("' ||"):
        ind = lines[i][: len(lines[i]) - len(lines[i].lstrip())]
        out.append(ind + "while (!line.empty() && (line.back() == '\\r' || line.back() == '\\n'))\n")
        out.append(ind + "    line.pop_back();\n")
        i += 2
        if i < len(lines) and lines[i].strip() == "line.pop_back();":
            i += 1
        continue
    if lines[i].lstrip().startswith("' || line.back()"):
        i += 1
        continue
    out.append(lines[i])
    i += 1
text = "".join(out)
p.write_text(text, encoding="utf-8", newline="\n")
print("fixed", p.stat().st_size)