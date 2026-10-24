// Fake mirrored iPhone (portrait, Fit => pillarboxed) used by the mirroring-state mockups.
// Usage: <div id="video"></div><script src="phone.js"></script>
(function () {
  var c = ['#ff9f0a', '#30d158', '#0a84ff', '#bf5af2', '#ff375f', '#64d2ff', '#ffd60a', '#5e5ce6',
           '#ac8e68', '#ff453a', '#32ade6', '#66d4cf', '#8e8e93', '#30b0c7', '#ff9f0a', '#34c759'];
  var grid = c.map(function (x) { return '<i style="background:' + x + '"></i>'; }).join('');
  var dock = c.slice(0, 4).map(function (x) { return '<i style="background:' + x + '"></i>'; }).join('');
  var css = '' +
    '#video{position:absolute;inset:0;background:#000}' +
    '.phone{position:absolute;top:0;left:711px;width:498px;height:1080px;overflow:hidden;' +
      'background:linear-gradient(160deg,#1d3b6e 0%,#3d2a6b 45%,#7a3f5e 80%,#a85a4a 100%)}' +
    '.ph-status{display:flex;justify-content:space-between;align-items:center;padding:26px 40px 0;font:600 22px/1 var(--font)}' +
    '.ph-pill{width:120px;height:34px;border-radius:17px;background:#000}' +
    '.ph-grid{display:grid;grid-template-columns:repeat(4,86px);gap:38px 26px;justify-content:center;margin-top:70px}' +
    '.ph-grid i,.ph-dock i{display:block;width:86px;height:86px;border-radius:22px;opacity:.92}' +
    '.ph-dock{position:absolute;left:24px;right:24px;bottom:30px;height:124px;border-radius:40px;' +
      'background:rgba(255,255,255,.18);display:flex;justify-content:space-around;align-items:center;padding:0 18px}';
  document.head.insertAdjacentHTML('beforeend', '<style>' + css + '</style>');
  document.getElementById('video').innerHTML =
    '<div class="phone"><div class="ph-status"><span>9:41</span><span class="ph-pill"></span><span>100%</span></div>' +
    '<div class="ph-grid">' + grid + '</div><div class="ph-dock">' + dock + '</div></div>';
})();
