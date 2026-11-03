// CamViewCommand.h : the regular command that draws CAM graphics and receives picks.
//
// Alibre only draws add-on graphics while a regular command is active, and any
// native command ends it. The CAM tab starts this command when it gains focus.

#pragma once

#include "CommandBase.h"
#include "Origin.h"

class CCamTabCommand;

// What the view draws, in assembly millimetres. The tab builds it.
struct CamScene
{
    // Only the origin the selected step runs under, or the one being edited.
    bool hasOrigin = false;
    camcore::Frame origin;
    camcore::PlaneRectangle plane;

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
