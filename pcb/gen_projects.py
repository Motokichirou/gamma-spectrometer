"""
gen_projects.py — генерация стартовых KiCad 9.0 файлов для всех плат
Запуск: python pcb/gen_projects.py
"""

import json, os

BASE = os.path.dirname(os.path.abspath(__file__))

BOARDS = {
    "hv_board":        {"title": "HV Power Board",   "diameter": 45.0},
    "mcu_board":       {"title": "MCU/ADC Board v1.6","diameter": 45.0},
    "usb_power_board": {"title": "USB & Power Board", "diameter": 45.0},
    "divider_board":   {"title": "Divider + AD8000",  "diameter": 44.0},
}

DATE = "2026-05-15"
REV  = "1.0"
COMPANY = "Gamma Spectrometer"

# ──────────────────────────────────────────────
# .kicad_pro
# ──────────────────────────────────────────────
def make_pro(name, title):
    return {
        "board": {"design_settings": {}, "layer_presets": [], "viewports": []},
        "boards": [],
        "cvpcb": {"equivalence_files": []},
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": f"{name}.kicad_pro", "version": 1},
        "net_settings": {
            "classes": [{"clearance": 0.2, "diff_pair_gap": 0.25,
                          "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
                          "line_style": 0, "microvia_diameter": 0.3,
                          "microvia_drill": 0.1, "name": "Default",
                          "pcb_color": "rgba(0, 0, 0, 0.000)",
                          "schematic_color": "rgba(0, 0, 0, 0.000)",
                          "track_width": 0.25, "via_diameter": 0.8,
                          "via_drill": 0.4, "wire_width": 6}],
            "meta": {"version": 3},
            "net_colors": {}
        },
        "pcbnew": {"last_paths": {}, "page_layout_descr_file": ""},
        "schematic": {
            "annotate_start_num": 0,
            "drawing": {"default_line_thickness": 6, "default_text_size": 50,
                         "field_names": [], "intersheets_ref_own_page": False,
                         "intersheets_ref_prefix": "", "intersheets_ref_short": False,
                         "intersheets_ref_show": False, "intersheets_ref_suffix": "",
                         "junction_size_choice": 3, "label_size_ratio": 0.375,
                         "operating_point_overlay_i_precision": 3,
                         "operating_point_overlay_i_range": "~A",
                         "operating_point_overlay_v_precision": 3,
                         "operating_point_overlay_v_range": "~V",
                         "overbar_offset_ratio": 1.23,
                         "pin_symbol_size": 25, "text_offset_ratio": 0.08},
            "legacy_lib_dir": "", "legacy_lib_list": [],
            "meta": {"version": 1},
            "net_format_name": "",
            "page_layout_descr_file": "",
            "plot_directory": "",
            "spice_current_sheet_as_root": False,
            "subpart_first_id": 65, "subpart_id_separator": 0
        },
        "sheets": [[f"{name}.kicad_sch", ""]],
        "text_variables": {}
    }

# ──────────────────────────────────────────────
# .kicad_sch  (минимальная схема)
# ──────────────────────────────────────────────
def make_sch(title, rev=REV, date=DATE, company=COMPANY):
    return f"""\
(kicad_sch
  (version 20231120)
  (generator "eeschema")
  (generator_version "9.0")
  (paper "A4")
  (title_block
    (title "{title}")
    (date "{date}")
    (rev "{rev}")
    (company "{company}")
  )
  (lib_symbols)
  (symbol_instances)
)
"""

# ──────────────────────────────────────────────
# .kicad_pcb  (пустая плата с круглым контуром)
# ──────────────────────────────────────────────
def make_pcb(title, diameter, rev=REV, date=DATE, company=COMPANY):
    r = diameter / 2.0
    cx, cy = 100.0, 100.0   # центр платы на поле

    layers = "\n".join([
        '    (0 "F.Cu" signal)',
        '    (31 "B.Cu" signal)',
        '    (32 "B.Adhes" user "B.Adhesive")',
        '    (33 "F.Adhes" user "F.Adhesive")',
        '    (34 "B.Paste" user)',
        '    (35 "F.Paste" user)',
        '    (36 "B.SilkS" user "B.Silkscreen")',
        '    (37 "F.SilkS" user "F.Silkscreen")',
        '    (38 "B.Mask" user)',
        '    (39 "F.Mask" user)',
        '    (40 "Dwgs.User" user "User.Drawings")',
        '    (41 "Cmts.User" user "User.Comments")',
        '    (42 "Eco1.User" user "User.Eco1")',
        '    (43 "Eco2.User" user "User.Eco2")',
        '    (44 "Edge.Cuts" user)',
        '    (45 "Margin" user)',
        '    (46 "B.CrtYd" user "B.Courtyard")',
        '    (47 "F.CrtYd" user "F.Courtyard")',
        '    (48 "B.Fab" user "B.Fabrication")',
        '    (49 "F.Fab" user "F.Fabrication")',
    ])

    return f"""\
(kicad_pcb
  (version 20240108)
  (generator "pcbnew")
  (generator_version "9.0")
  (general
    (thickness 1.6)
    (legacy_teardrops no)
  )
  (paper "A4")
  (title_block
    (title "{title}")
    (date "{date}")
    (rev "{rev}")
    (company "{company}")
  )
  (layers
{layers}
  )
  (setup
    (pad_to_mask_clearance 0.05)
    (solder_mask_min_width 0)
    (allow_soldermask_bridges_in_footprints no)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (disableapertmacros no)
      (usegerberextensions no)
      (usegerberattributes yes)
      (usegerberadvancedattributes yes)
      (creategerberjobfile yes)
      (dashed_line_dash_ratio 12.0)
      (dashed_line_gap_ratio 3.0)
      (svgprecision 4)
      (plotframeref no)
      (viasonmask no)
      (mode 1)
      (useauxorigin no)
      (hpglpennumber 1)
      (hpglpenspeed 20)
      (hpglpendiameter 15.0)
      (pdf_front_fp_property_popups yes)
      (pdf_back_fp_property_popups yes)
      (dxfpolygonmode yes)
      (dxfimperialunits yes)
      (dxfusepcbnewfont yes)
      (psnegative no)
      (psa4output no)
      (plotreference yes)
      (plotvalue yes)
      (plotfptext yes)
      (plotinvisibletext no)
      (sketchpadsonfab no)
      (subtractmaskfromsilk no)
      (outputformat 1)
      (mirror no)
      (drillshape 0)
      (scaleselection 1)
      (outputdirectory "gerber/")
    )
  )
  (net 0 "")
  (gr_circle
    (center {cx} {cy})
    (end {cx + r} {cy})
    (stroke (width 0.05) (type solid))
    (fill (type none))
    (layer "Edge.Cuts")
  )
  (gr_text "Ø{diameter:.0f}mm"
    (at {cx} {cy - r - 3})
    (layer "F.SilkS")
    (effects (font (size 1.5 1.5) (thickness 0.15)))
  )
)
"""

# ──────────────────────────────────────────────
# Запись файлов
# ──────────────────────────────────────────────
for board_dir, info in BOARDS.items():
    name  = board_dir
    title = info["title"]
    diam  = info["diameter"]
    out   = os.path.join(BASE, board_dir)
    os.makedirs(out, exist_ok=True)

    # .kicad_pro
    pro_path = os.path.join(out, f"{name}.kicad_pro")
    with open(pro_path, "w", encoding="utf-8") as f:
        json.dump(make_pro(name, title), f, indent=2)

    # .kicad_sch
    sch_path = os.path.join(out, f"{name}.kicad_sch")
    with open(sch_path, "w", encoding="utf-8") as f:
        f.write(make_sch(title))

    # .kicad_pcb
    pcb_path = os.path.join(out, f"{name}.kicad_pcb")
    with open(pcb_path, "w", encoding="utf-8") as f:
        f.write(make_pcb(title, diam))

    print(f"OK {board_dir}/  ({diam:.0f}mm)")

print("\nDone. Open any .kicad_pro in KiCad 9.")
