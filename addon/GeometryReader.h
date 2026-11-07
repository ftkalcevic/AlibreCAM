// GeometryReader.h : reads picked faces and edges, and the assembly's size, into CamCore's terms.
//
// Alibre works in each file's model units, in that file's own coordinates.
// Everything returned here is in assembly coordinates and millimetres.

#pragma once

#include <memory>

#include "Boundary.h"
#include "Features.h"
#include "Origin.h"
#include "Stock.h"

// What a Set Origin pick may be.
enum class PickFilter
{
    OriginPlane,        // a flat or cylindrical face, a straight or circular edge, a design plane or axis
    OriginPosition,     // a straight or circular edge, or a design point
    OriginX             // a straight edge or a design axis
};

// Millimetres per model unit for a session, or 10 (centimetres) if it can't be read.
double MillimetresPerModelUnit(IADDesignSession* pDesign);

// Reads the first selected item. Returns false and sets problem, in words for the
// user, if nothing usable is selected.
bool ReadSelectedPick(IADSession* pAssembly, PickFilter filter, camcore::PickedGeometry& pick,
                      std::wstring& problem);

// As ReadSelectedPick, for measuring a depth: a design point or plane, a face, or an edge.
bool ReadSelectedLocate(IADSession* pAssembly, camcore::LocatedGeometry& located, std::wstring& problem);

// Reads every selected item the filter allows, in selection order, and counts
// the ones it doesn't. Returns false only if the selection can't be read.
bool ReadSelectedFeatures(IADSession* pAssembly, camcore::FeatureFilter filter,
                          std::vector<camcore::PickedGeometry>& features, int& ignored);

// Replaces Alibre's selection with the features, found again by their keys.
// Returns how many were selected. Design points and deleted items can't be.
int SelectFeatures(IADSession* pAssembly, const std::vector<camcore::PickedGeometry>& features);

// A face's outer loop (or with allLoops, every loop's edges, holes too), or an
// edge, as pieces of polyline in assembly mm, found again by the feature's keys.
// Each piece is one edge, from its start to its end. Returns false and sets
// problem if it can't be found or read.
bool ReadFeatureOutline(IADSession* pAssembly, const camcore::PickedGeometry& feature,
                        std::vector<camcore::Path>& pieces, std::wstring& problem, bool allLoops = false);

// Highlights one feature in the view without changing the selection. Null clears it.
void HighlightFeature(IADSession* pAssembly, const camcore::PickedGeometry* pFeature);

// The assembly's extents. Returns false if it has no parts or they can't be read.
bool ReadAssemblyBounds(IADSession* pAssembly, camcore::Vec3& lower, camcore::Vec3& upper);

// The parts of Alibre's selection filter a pick sets.
struct SelectionFilter
{
    ADSelectionFilterOption solid = ADSelectionFilterOption_AD_FACES_EDGES;
    bool planes = false;
    bool axes = false;
    bool points = false;
};

// Sets Alibre's selection filter so only what the pick allows can be clicked.
// Returns the previous setting so it can be put back.
SelectionFilter SetSelectionFilter(IADSession* pAssembly, const SelectionFilter& wanted);

void ClearSelection(IADSession* pAssembly);

// The parts behind every selected item (a part, or a face or edge of one), in
// selection order, each once. Returns false only if the selection can't be read.
bool ReadSelectedParts(IADSession* pAssembly, std::vector<camcore::StockPart>& parts);

// The box around some parts, found again by their keys, or around the whole
// assembly when the list is empty. Returns false and sets problem if a part
// can't be found or nothing can be measured.
bool ReadPartsBounds(IADSession* pAssembly, const std::vector<camcore::StockPart>& parts, camcore::Vec3& lower,
                     camcore::Vec3& upper, std::wstring& problem);

// Opens an Alibre part without showing it, reads its facets (three points to a
// triangle, in the part's own coordinates in mm), and closes it. Returns false
// and sets problem if the file can't be opened or isn't a part.
bool ReadPartFacets(const std::wstring& path, std::vector<camcore::Vec3>& triangles, std::wstring& problem);

// A picked face's facets (three points to a triangle) in the assembly's
// coordinates in mm, about 0.05 mm from the true surface. Returns false and sets
// problem if the face can't be found or read.
bool ReadFaceFacets(IADSession* pAssembly, const camcore::PickedGeometry& feature,
                    std::vector<camcore::Vec3>& triangles, std::wstring& problem);

// The facets of every visible, unsuppressed part in the assembly (three points to a
// triangle, in the assembly's coordinates in mm), leaving out the part whose file is
// leaveOutPath (an imported stock's). Each part is read from its occurrence's own
// session, or from its file if that fails. Returns false and sets problem if no
// part could be read.
bool ReadAssemblyPartFacets(IADSession* pAssembly, const std::wstring& leaveOutPath,
                            std::vector<camcore::Vec3>& triangles, std::wstring& problem);

// Every visible part's faces, each read with its own facets and kept with its
// true surface, so a tool's contacts can be moved onto the surface.
class PartSurfaces
{
public:
    PartSurfaces();
    ~PartSurfaces();

    // Reads every visible, unsuppressed part (less leaveOutPath's) face by face:
    // triangles in the assembly's coordinates in mm, and the face each is on (-1
    // for a plane, whose triangles are already exact, or where a part could only
    // be read whole). Returns false and sets problem if no part could be read.
    bool Read(IADSession* pAssembly, const std::wstring& leaveOutPath, std::vector<camcore::Vec3>& triangles,
              std::vector<int>& faces, std::wstring& problem);

    // The closest point on a face's true surface to p, both in the assembly's
    // coordinates in mm, and whether it's on the face itself. False if Alibre
    // can't say.
    bool ClosestPoint(int face, const camcore::Vec3& p, camcore::Vec3& closest, bool& onFace) const;

    // Logs the first time each way ClosestPoint failed, and how often.
    void LogFailures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
