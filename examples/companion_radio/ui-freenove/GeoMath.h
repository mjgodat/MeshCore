#pragma once

// Great-circle helpers for the visual head (pure, header-only, C++11).
// Coordinates are integer micro-degrees (lat/lon * 1e6), as carried by MeshCore
// adverts and observer vitals.

#include <math.h>
#include <stdint.h>

namespace mesh { namespace head {

static const double GEO_EARTH_RADIUS_KM = 6371.0088;   // IUGG mean radius
static const double GEO_PI = 3.14159265358979323846;

inline double geoRad(int32_t e6) { return ((double)e6 / 1e6) * GEO_PI / 180.0; }

inline bool geoValid(int32_t lat_e6, int32_t lon_e6) {
  return lat_e6 >= -90000000 && lat_e6 <= 90000000 && lon_e6 >= -180000000 && lon_e6 <= 180000000;
}

// Haversine distance in km.
inline double distanceKm(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6) {
  const double p1 = geoRad(lat1_e6), p2 = geoRad(lat2_e6);
  const double dp = p2 - p1, dl = geoRad(lon2_e6) - geoRad(lon1_e6);
  const double s1 = sin(dp / 2), s2 = sin(dl / 2);
  double a = s1 * s1 + cos(p1) * cos(p2) * s2 * s2;
  if (a > 1.0) a = 1.0;
  if (a < 0.0) a = 0.0;
  return 2.0 * GEO_EARTH_RADIUS_KM * atan2(sqrt(a), sqrt(1.0 - a));
}

// Initial great-circle bearing from point 1 to point 2, degrees in [0, 360).
inline double bearingDeg(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6) {
  const double p1 = geoRad(lat1_e6), p2 = geoRad(lat2_e6);
  const double dl = geoRad(lon2_e6) - geoRad(lon1_e6);
  const double y = sin(dl) * cos(p2);
  const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double deg = atan2(y, x) * 180.0 / GEO_PI;
  deg = fmod(deg + 360.0, 360.0);
  if (deg >= 360.0) deg = 0.0;
  return deg;
}

}}  // namespace mesh::head
