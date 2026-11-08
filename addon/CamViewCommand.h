// CamViewCommand.h : the regular command that draws CAM graphics and receives picks.
//
// Alibre only draws add-on graphics while a regular command is active, and any
// native command ends it. The CAM tab starts this command when it gains focus.

#pragma once

#include "CommandBase.h"
#include "Origin.h"
#include "Simulation.h"

#include <vector>

class CCamTabCommand;

// What the view draws, in assembly millimetres. The tab builds it.
struct CamScene
{
    // Only the origin the selected step runs under, or the one being edited.
    bool hasOrigin = false;

    // Alibre picks the plane's mesh instead of the model behind it, so the
    // plane is left out while the user picks in the view.
    bool showPlane = true;
    camcore::Frame origin;
    camcore::PlaneRectangle plane;

    // Each operation's toolpath as runs of connected lines, already in assembly
    // coordinates. Left out while picking, like the plane.
    struct Toolpath
    {
        std::vector<std::vector<camcore::Vec3>> rapids;
        std::vector<std::vector<camcore::Vec3>> feeds;
        std::vector<std::vector<camcore::Vec3>> arcs;      // arc feeds, sampled
        bool selected = false;
        bool suppressed = false;    // drawn thin and grey
    };
    std::vector<Toolpath> toolpaths;
    std::vector<std::vector<camcore::Vec3>> boundaries;     // facing areas, closed
    std::vector<std::vector<camcore::Vec3>> highlights;     // the chosen operation's features
    bool showToolpaths = true;      // the boundaries too. Off while picking.
    bool toolpathsShown = true;     // the toolbar's Show Toolpaths, for the toolpaths alone

    // The stock's edges, in assembly mm. Left out while picking, like the plane.
    std::vector<camcore::LineSegment> stockEdges;
    bool showStock = true;          // off while picking
    bool stockShown = true;         // the toolbar's Show Stock
    bool originShown = true;        // the toolbar's Show Origin: the active origin's axes and plane

    // The simulation, while its dialog is open: the cut stock as one mesh in
    // assembly mm (in place of the stock's edges), the tool where it is, and a
    // rapid that cut stock.
    bool simulating = false;
    const camcore::StockMesh* simMesh = nullptr;
    bool simStockShown = true;                          // the dialog's Show Stock

    // Comparing with the parts, each drawn when it's set: the parts' cells, stock
    // left that should have been cut, and part material cut away.
    const camcore::StockMesh* simPartMesh = nullptr;
    const camcore::StockMesh* simNotEnoughMesh = nullptr;
    const camcore::StockMesh* simTooMuchMesh = nullptr;
    bool simToolShown = false;
    camcore::Vec3 simTip;
    camcore::Vec3 simAxis{ 0.0, 0.0, 1.0 };
    camcore::ToolProfile simProfile;
    camcore::SimCylinder simHolder;                     // drawn if present
    camcore::SimCylinder simShank;                      // drawn if wider than the tool (a reduced neck)
    bool simWarning = false;
    camcore::Vec3 simWarningFrom;
    camcore::Vec3 simWarningTo;

    bool foreground = true;     // drawn over the model, rather than hidden by it
    double millimetresPerUnit = 10.0;
};

class CCamViewCommand : public CCommandBase
{
public:
    explicit CCamViewCommand(CCamTabCommand* pTab);
    virtual ~CCamViewCommand();

    // Asks Alibre to redraw, which calls On3DRender.
    void Refresh();

    // IAlibreAddOnCommand
    STDMETHOD(OnSelectionChange)() override;
    STDMETHOD(OnTerminate)() override;
    STDMETHOD(OnComplete)() override;
    STDMETHOD(On3DRender)() override;

private:
    void DrawScene(IADAddOnCanvasDisplay* pCanvas, const CamScene& scene);

    CCamTabCommand* m_pTab;     // holds a reference until OnTerminate
    bool m_reportedRender;
};
