#pragma once

// Lat/lon -> screen projection for the HOME GEO view (pure, header-only,
// C++11; host-tested by test/test_layouts.py).
//
// Local equirectangular projection centred on the repeater (north up),
// accurate to well under 1 % across the few tens of km a LoRa mesh spans.
// fit() picks the zoom that puts every point within kMaxFitKm of the centre
// inside the area minus a margin, keeping the repeater at the exact centre
// (so the scale is set by the farthest point along each axis). Points
// farther than kMaxFitKm (bad fixes, far-away adverts) are left out of the
// fit and do not project.

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace freenove_ui {

struct GeoPoint {
  int32_t lat_e6, lon_e6;
};

class GeoLayout {
 public:
  static constexpr double kMaxFitKm = 300.0;
  static constexpr double kDefaultPxPerKm = 20.0;   // nothing to fit
  static constexpr double kMaxPxPerKm = 2000.0;     // ~0.5 m per px

  GeoLayout(int w = 340, int h = 270, int margin = 14)
      : _w(w), _h(h), _margin(margin), _lat0(0), _lon0(0), _cosLat(1.0), _scale(kDefaultPxPerKm) {}

  int width() const { return _w; }
  int height() const { return _h; }
  int cx() const { return _w / 2; }
  int cy() const { return _h / 2; }
  double pxPerKm() const { return _scale; }

  // East/north offset in km from (lat0, lon0).
  static void offsetKm(int32_t lat0, int32_t lon0, int32_t lat, int32_t lon, double cos_lat0,
                       double& east, double& north) {
    double dlon = (double)lon - (double)lon0;
    if (dlon > 180e6) dlon -= 360e6;
    if (dlon < -180e6) dlon += 360e6;
    north = ((double)lat - (double)lat0) / 1e6 * kKmPerDegLat;
    east = dlon / 1e6 * kKmPerDegLat * cos_lat0;
  }

  void fit(int32_t lat0, int32_t lon0, const GeoPoint* pts, size_t n) {
    _lat0 = lat0;
    _lon0 = lon0;
    _cosLat = cos((double)lat0 / 1e6 * 3.14159265358979323846 / 180.0);
    if (_cosLat < 0.01) _cosLat = 0.01;
    double maxE = 0, maxN = 0;
    for (size_t i = 0; pts != nullptr && i < n; i++) {
      double e, no;
      offsetKm(lat0, lon0, pts[i].lat_e6, pts[i].lon_e6, _cosLat, e, no);
      if (sqrt(e * e + no * no) > kMaxFitKm) continue;
      if (fabs(e) > maxE) maxE = fabs(e);
      if (fabs(no) > maxN) maxN = fabs(no);
    }
    const double halfW = (double)_w / 2.0 - _margin, halfH = (double)_h / 2.0 - _margin;
    double s = kMaxPxPerKm;
    if (maxE > 0 && halfW / maxE < s) s = halfW / maxE;
    if (maxN > 0 && halfH / maxN < s) s = halfH / maxN;
    if (maxE <= 0 && maxN <= 0) s = kDefaultPxPerKm;
    if (s < 0.0001) s = 0.0001;
    _scale = s;
  }

  // Screen position (area coordinates); false when beyond the fitted range.
  bool project(int32_t lat, int32_t lon, int& x, int& y) const {
    double e, no;
    offsetKm(_lat0, _lon0, lat, lon, _cosLat, e, no);
    if (sqrt(e * e + no * no) > kMaxFitKm) return false;
    const double fx = (double)cx() + e * _scale, fy = (double)cy() - no * _scale;
    if (fx < 0 || fy < 0 || fx > _w - 1 || fy > _h - 1) return false;
    x = (int)lround(fx);
    y = (int)lround(fy);
    return true;
  }

  double distanceKm(int32_t lat, int32_t lon) const {
    double e, no;
    offsetKm(_lat0, _lon0, lat, lon, _cosLat, e, no);
    return sqrt(e * e + no * no);
  }

  // A round range-ring distance (1, 2, 5 x 10^n km) of at most max_km.
  static double niceKm(double max_km) {
    if (!(max_km > 0)) return 0;
    double p = pow(10.0, floor(log10(max_km)));
    if (5 * p <= max_km) return 5 * p;
    if (2 * p <= max_km) return 2 * p;
    return p;
  }

 private:
  static constexpr double kKmPerDegLat = 111.195;   // mean Earth radius 6371 km

  int _w, _h, _margin;
  int32_t _lat0, _lon0;
  double _cosLat;
  double _scale;
};

}  // namespace freenove_ui
