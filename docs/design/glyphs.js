// Controller glyph stand-ins for the mockups. The app draws these with the system icon font
// (FontFamily "Segoe Fluent Icons, Segoe MDL2 Assets"): ButtonA F093, ButtonB F094, ButtonY F095, ButtonX F096,
// ButtonMenu EDE3, ButtonView2 EECA, TriggerLeft F10A, TriggerRight F10B, BumperLeft F10C, BumperRight F10D, Dpad F10E.
// Linux has neither font, so this replaces <i class="g" data-g="A"></i> with a plain SVG outline of the same idea
// (circle + letter, bumper, trigger, three lines, two squares). Usage: <script src="glyphs.js"></script> at the end of body.
(function () {
  var S = 'fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round"';
  function txt(t, size, y) {
    return '<text x="16" y="' + y + '" text-anchor="middle" font-family="Segoe UI, Lato, sans-serif" font-weight="700" ' +
      'font-size="' + size + '" fill="currentColor">' + t + '</text>';
  }
  var face = function (l) { return '<circle cx="16" cy="16" r="13" ' + S + '/>' + txt(l, 14, 21); };
  var shapes = {
    A: face('A'), B: face('B'), X: face('X'), Y: face('Y'),
    Menu: '<circle cx="16" cy="16" r="13" ' + S + '/><path d="M10.5 11.5h11M10.5 16h11M10.5 20.5h11" ' + S + '/>',
    View: '<circle cx="16" cy="16" r="13" ' + S + '/><rect x="9" y="10" width="9" height="7.5" rx="1.6" ' + S +
      '/><rect x="14" y="14.5" width="9" height="7.5" rx="1.6" ' + S + '/>',
    LB: '<path d="M2.5 23 C2.5 13 7 9.5 13 9.5 H19 C25 9.5 29.5 13 29.5 23 Z" ' + S + '/>' + txt('LB', 9.5, 20.5),
    RB: '<path d="M2.5 23 C2.5 13 7 9.5 13 9.5 H19 C25 9.5 29.5 13 29.5 23 Z" ' + S + '/>' + txt('RB', 9.5, 20.5),
    LT: '<path d="M9 3.5 H20 C24 3.5 26 6.5 26 11 V22 C26 26 23.5 28.5 19.5 28.5 H12.5 C8.5 28.5 6 26 6 22 V7.5 C6 5 7 3.5 9 3.5 Z" ' +
      S + '/>' + txt('LT', 9.5, 20),
    RT: '<path d="M23 3.5 H12 C8 3.5 6 6.5 6 11 V22 C6 26 8.5 28.5 12.5 28.5 H19.5 C23.5 28.5 26 26 26 22 V7.5 C26 5 25 3.5 23 3.5 Z" ' +
      S + '/>' + txt('RT', 9.5, 20),
    Dpad: '<path d="M12.5 3.5h7v9h9v7h-9v9h-7v-9h-9v-7h9z" ' + S + '/>'
  };
  document.head.insertAdjacentHTML('beforeend',
    '<style>i.g{display:inline-flex;width:1.35em;height:1.35em;vertical-align:middle;flex:none}i.g svg{width:100%;height:100%}</style>');
  var els = document.querySelectorAll('i.g');
  for (var i = 0; i < els.length; i++) {
    var k = els[i].getAttribute('data-g');
    els[i].innerHTML = '<svg viewBox="0 0 32 32">' + (shapes[k] || txt('?', 14, 21)) + '</svg>';
  }
})();
