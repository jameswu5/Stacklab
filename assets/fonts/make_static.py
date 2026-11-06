# Regenerates the static fonts. SDL_ttf can't select a variable-font weight or
# an OpenType feature, so each weight is instanced with tabular digits in cmap.
# Needs: pip install fonttools. Run from this directory.
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

for name, weight in [("Regular", 400), ("SemiBold", 600), ("Bold", 700)]:
    font = instantiateVariableFont(TTFont("PlusJakartaSans[wght].ttf"), {"wght": weight})
    gsub = font["GSUB"].table
    tnum = {}
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag == "tnum":
            for index in record.Feature.LookupListIndex:
                for table in gsub.LookupList.Lookup[index].SubTable:
                    tnum.update(getattr(table, "ExtSubTable", table).mapping)
    for table in font["cmap"].tables:
        for code, glyph in table.cmap.items():
            if ord("0") <= code <= ord("9"):
                table.cmap[code] = tnum.get(glyph, glyph)  # cmap tables can share a dict
    font.save(f"PlusJakartaSans-{name}.ttf")
