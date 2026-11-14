"""Aggiorna la pagina gzip nello sketch, senza dipendenze aggiuntive."""
import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / 'firmware/DaikinWemos/DaikinWemos.ino'
BEGIN = '// BEGIN GENERATED WEB GZIP'
END = '// END GENERATED WEB GZIP'


def main():
    source = SKETCH.read_text(encoding='utf-8')
    page = source.split('R"HTML(', 1)[1].split(')HTML"', 1)[0].encode('utf-8')
    compressed = gzip.compress(page, compresslevel=9, mtime=0)
    rows = ['  ' + ','.join(f'0x{byte:02x}' for byte in compressed[i:i+16]) + ','
            for i in range(0, len(compressed), 16)]
    block = BEGIN + '\nstatic const uint8_t PAGE_GZIP[] PROGMEM = {\n' + '\n'.join(rows) + '\n};\n' + END
    if BEGIN in source:
        before, rest = source.split(BEGIN, 1)
        source = before + block + rest.split(END, 1)[1]
    else:
        marker = '\nuint32_t configChecksum(const void *record, size_t size) {'
        assert marker in source
        source = source.replace(marker, '\n' + block + '\n' + marker, 1)
    SKETCH.write_text(source, encoding='utf-8', newline='\n')
    print(f'Pagina web: {len(page)} -> {len(compressed)} byte gzip')


if __name__ == '__main__':
    main()
