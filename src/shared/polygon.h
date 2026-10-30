#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Zone vertices are local metres, relative to the zone centre and rotated by yaw.
namespace polygon {
struct Point { double x=0, y=0; };
inline double Cross(Point a, Point b, Point p) { return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x); }
inline bool OnEdge(Point a, Point b, Point p) {
    const double length=std::hypot(b.x-a.x,b.y-a.y);
    // One millimetre absorbs float world-coordinate quantisation on rotated edges.
    return std::abs(Cross(a,b,p))<=.001*std::max(1.0,length)
        && p.x>=std::min(a.x,b.x)-.001 && p.x<=std::max(a.x,b.x)+.001
        && p.y>=std::min(a.y,b.y)-.001 && p.y<=std::max(a.y,b.y)+.001;
}
inline bool Intersects(Point a, Point b, Point c, Point d) {
    if(OnEdge(a,b,c)||OnEdge(a,b,d)||OnEdge(c,d,a)||OnEdge(c,d,b)) return true;
    return (Cross(a,b,c)>0)!=(Cross(a,b,d)>0) && (Cross(c,d,a)>0)!=(Cross(c,d,b)>0);
}
inline std::string Validate(const std::vector<Point>& points) {
    const size_t n=points.size();
    if(n<3 || n>128) return "A polygon needs 3 to 128 vertices";
    double area=0;
    for(size_t i=0;i<n;++i) {
        const auto a=points[i], b=points[(i+1)%n];
        if(!std::isfinite(a.x)||!std::isfinite(a.y)||std::abs(a.x)>100000||std::abs(a.y)>100000) return "Invalid polygon coordinates";
        if(std::hypot(b.x-a.x,b.y-a.y)<.01) return "Polygon vertices must be at least 1 cm apart";
        area+=a.x*b.y-b.x*a.y;
        const auto c=points[(i+2)%n];
        if(OnEdge(a,b,c)||OnEdge(b,c,a)) return "Polygon edges overlap";
        for(size_t j=i+1;j<n;++j) {
            if(j==i+1 || (i==0 && j==n-1)) continue;
            if(Intersects(a,b,points[j],points[(j+1)%n])) return "Polygon edges intersect";
        }
    }
    if(std::abs(area)<.0001) return "Polygon area is zero";
    return {};
}
inline bool Contains(const std::vector<Point>& points, Point p) {
    if(points.size()<3 || !std::isfinite(p.x)||!std::isfinite(p.y)) return false;
    bool inside=false;
    for(size_t i=0,j=points.size()-1;i<points.size();j=i++) {
        const auto a=points[j],b=points[i];
        if(OnEdge(a,b,p)) return true; // boundaries belong to the safe zone
        if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x) inside=!inside;
    }
    return inside;
}
}
