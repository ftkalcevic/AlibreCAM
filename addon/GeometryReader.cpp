// GeometryReader.cpp : reads picked faces and edges, and the assembly's size, into CamCore's terms.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "GeometryReader.h"
#include "ModelReader.h"
#include "Log.h"

#include <cmath>

using camcore::PickedGeometry;
using camcore::PickKind;
using camcore::Vec3;

namespace
{
    // A circle's axis and a face normal closer than this are taken as parallel.
    const double kParallelDot = 0.999;

    // QueryInterface into a smart pointer, giving an empty pointer on failure.
    template <class TPtr>
    TPtr QI(IUnknown* pUnknown)
    {
        TPtr result;
        if (pUnknown != nullptr)
            pUnknown->QueryInterface(TPtr::GetIID(), reinterpret_cast<void**>(&result));
        return result;
    }

    Vec3 ToVec3(IADPoint* pPoint)
    {
        return { pPoint->GetX(), pPoint->GetY(), pPoint->GetZ() };
    }

    Vec3 ToVec3(IADVector* pVector)
    {
        return { pVector->GetX(), pVector->GetY(), pVector->GetZ() };
    }

    camcore::PersistentKey TakeKey(SAFEARRAY* pArray)
    {
        camcore::PersistentKey key;
        if (pArray == nullptr)
            return key;

        VARTYPE vt = VT_EMPTY;
        SafeArrayGetVartype(pArray, &vt);
        key.elementType = vt;

        LONG lower = 0, upper = -1;
        SafeArrayGetLBound(pArray, 1, &lower);
        SafeArrayGetUBound(pArray, 1, &upper);
        const size_t bytes = static_cast<size_t>(upper - lower + 1) * SafeArrayGetElemsize(pArray);

        BYTE* pData = nullptr;
        if (bytes > 0 && SUCCEEDED(SafeArrayAccessData(pArray, reinterpret_cast<void**>(&pData))))
        {
            static const char digits[] = "0123456789ABCDEF";
            key.hex.reserve(bytes * 2);
            for (size_t i = 0; i < bytes; ++i)
            {
                key.hex += digits[pData[i] >> 4];
                key.hex += digits[pData[i] & 0x0F];
            }
            SafeArrayUnaccessData(pArray);
        }
        SafeArrayDestroy(pArray);
        return key;
    }

    // A part's coordinates and units, and how to bring them into the assembly.
    // The world transform is by rows, with the translation in the fourth row,
    // and works in the assembly's units. No occurrence means the item belongs to
    // the assembly itself, so it's already in assembly coordinates.
    class PartToAssembly
    {
    public:
        PartToAssembly(IADOccurrence* pOccurrence, double assemblyMm)
            : m_partToAssembly(1.0)
            , m_assemblyMm(assemblyMm)
        {
            if (pOccurrence == nullptr)
            {
                m_matrix[0] = m_matrix[5] = m_matrix[10] = m_matrix[15] = 1.0;
                return;
            }

            const double partMm = MillimetresPerModelUnit(pOccurrence->GetDesignSession());
            m_partToAssembly = partMm / assemblyMm;
            if (std::fabs(partMm - assemblyMm) > 1e-9)
                Log(L"[pick] part units (%g mm) differ from the assembly's (%g mm)", partMm, assemblyMm);

            SAFEARRAY* pArray = pOccurrence->GetWorldTransform()->Array();
            double* pData = nullptr;
            if (pArray == nullptr || FAILED(SafeArrayAccessData(pArray, reinterpret_cast<void**>(&pData))))
                _com_issue_error(E_FAIL);
            std::copy(pData, pData + 16, m_matrix);
            SafeArrayUnaccessData(pArray);
            SafeArrayDestroy(pArray);
        }

        Vec3 Point(const Vec3& part) const
        {
            const Vec3 p = part * m_partToAssembly;
            const double* m = m_matrix;
            const Vec3 assembly{ p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
                                 p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
                                 p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14] };
            return assembly * m_assemblyMm;
        }

        Vec3 Direction(const Vec3& part) const
        {
            const double* m = m_matrix;
            const Vec3 assembly{ part.x * m[0] + part.y * m[4] + part.z * m[8],
                                 part.x * m[1] + part.y * m[5] + part.z * m[9],
                                 part.x * m[2] + part.y * m[6] + part.z * m[10] };
            Vec3 unit;
            return camcore::Normalise(assembly, unit) ? unit : assembly;
        }

    private:
        double m_matrix[16] = {};
        double m_partToAssembly;
        double m_assemblyMm;
    };

    // The normal pointing out of the material, in part coordinates. Returns
    // false if the face isn't flat.
    bool OutwardPlaneNormal(IADFace* pFace, Vec3& point, Vec3& normal)
    {
        IADPlanePtr plane = QI<IADPlanePtr>(pFace->GetGeometry());
        if (plane == nullptr)
            return false;

        point = ToVec3(plane->GetRootPoint());
        normal = ToVec3(plane->GetNormal());
        if (pFace->GetIsSenseReversed() != VARIANT_FALSE)
            normal = normal * -1.0;
        return true;
    }

    bool ReadFace(IADFace* pFace, PickedGeometry& pick)
    {
        Vec3 point, normal;
        if (OutwardPlaneNormal(pFace, point, normal))
        {
            pick.kind = PickKind::PlanarFace;
            pick.point = point;
            pick.direction = normal;
            Log(L"[pick] planar face, sense reversed=%d", pFace->GetIsSenseReversed() != VARIANT_FALSE ? 1 : 0);
        }
        else if (IADCylinderPtr cylinder = QI<IADCylinderPtr>(pFace->GetGeometry()))
        {
            pick.kind = PickKind::CylindricalFace;
            pick.point = ToVec3(cylinder->GetBasePoint());
            pick.direction = ToVec3(cylinder->GetAxis());
            Log(L"[pick] cylindrical face, radius %g", cylinder->GetRadius());
        }
        else
        {
            return false;
        }

        pick.targetKey = TakeKey(pFace->GetKey());
        return true;
    }

    // Turns a circle's axis to point out of the material, using a flat face the
    // edge borders whose normal is along the axis. Leaves it alone if there isn't one.
    void OrientCircleAxis(IADEdge* pEdge, Vec3& axis)
    {
        IADFacesPtr faces = pEdge->GetFaces();
        const long count = (faces != nullptr) ? faces->GetCount() : 0;
        for (long i = 0; i < count; ++i)
        {
            Vec3 point, normal;
            if (!OutwardPlaneNormal(faces->GetItem(i), point, normal))
                continue;

            Vec3 unitAxis, unitNormal;
            if (!camcore::Normalise(axis, unitAxis) || !camcore::Normalise(normal, unitNormal))
                continue;

            const double dot = camcore::Dot(unitAxis, unitNormal);
            if (std::fabs(dot) >= kParallelDot)
            {
                if (dot < 0.0)
                    axis = axis * -1.0;
                Log(L"[pick] circle axis taken from bordering face %ld%s", i, dot < 0.0 ? L", reversed" : L"");
                return;
            }
        }
        Log(L"[pick] circle axis left as Alibre gives it: no flat face along it");
    }

    bool ReadEdge(IADEdge* pEdge, PickedGeometry& pick)
    {
        IADCurvePtr curve = pEdge->GetGeometry();

        if (QI<IADLinePtr>(curve) != nullptr)
        {
            IADVertexPtr startVertex = pEdge->GetStartVertex();
            IADVertexPtr endVertex = pEdge->GetEndVertex();
            if (startVertex == nullptr || endVertex == nullptr)
                return false;

            pick.kind = PickKind::LineEdge;
            pick.start = ToVec3(startVertex->GetPoint());
            pick.end = ToVec3(endVertex->GetPoint());
            pick.point = pick.start;
            pick.direction = pick.end - pick.start;
        }
        else if (IADCirclePtr circle = QI<IADCirclePtr>(curve))
        {
            pick.kind = PickKind::CircleEdge;
            pick.point = ToVec3(circle->GetCenter());
            pick.direction = ToVec3(circle->GetAxis());
        }
        else if (IADCircularArcPtr arc = QI<IADCircularArcPtr>(curve))
        {
            pick.kind = PickKind::CircleEdge;
            pick.point = ToVec3(arc->GetCenter());
            pick.direction = ToVec3(arc->GetAxis());
        }
        else
        {
            return false;
        }

        if (pick.kind == PickKind::CircleEdge)
            OrientCircleAxis(pEdge, pick.direction);

        pick.targetKey = TakeKey(pEdge->GetKey());
        return true;
    }

    const wchar_t* AllowedText(PickFilter filter)
    {
        return (filter == PickFilter::Edges) ? L"Pick a straight or circular edge."
                                             : L"Pick a flat or cylindrical face, or a straight or circular edge.";
    }
}

double MillimetresPerModelUnit(IADDesignSession* pDesign)
{
    if (pDesign != nullptr)
    {
        switch (pDesign->GetDesignProperties()->GetModelUnits())
        {
        case ADUnits_AD_MILLIMETERS: return 1.0;
        case ADUnits_AD_CENTIMETERS: return 10.0;
        case ADUnits_AD_METERS:      return 1000.0;
        case ADUnits_AD_INCHES:      return 25.4;
        case ADUnits_AD_FEET:
        case ADUnits_AD_FEET_INCHES: return 304.8;
        default:                     break;
        }
    }
    Log(L"[model] model units unknown, assuming centimetres");
    return 10.0;
}

bool ReadSelectedPick(IADSession* pAssembly, PickFilter filter, PickedGeometry& pick, std::wstring& problem)
{
    pick = PickedGeometry();
    problem.clear();

    try
    {
        IADPointPtr clicked;
        IObjectCollectorPtr selected = pAssembly->SelectedObjectsEx(&clicked);
        const long count = (selected != nullptr) ? selected->GetCount() : 0;
        if (count == 0)
            return false;

        IADTargetProxyPtr proxy = QI<IADTargetProxyPtr>(selected->GetItem(_variant_t(0L)));
        IADOccurrencePtr occurrence = (proxy != nullptr) ? proxy->GetOccurrence() : nullptr;
        IDispatchPtr target = (proxy != nullptr) ? proxy->GetTarget() : nullptr;
        if (occurrence == nullptr || target == nullptr)
        {
            problem = AllowedText(filter);
            return false;
        }

        bool read = false;
        if (IADEdgePtr edge = QI<IADEdgePtr>(target))
            read = ReadEdge(edge, pick);
        else if (IADFacePtr face = QI<IADFacePtr>(target))
            read = (filter == PickFilter::FacesAndEdges) && ReadFace(face, pick);

        if (!read)
        {
            pick = PickedGeometry();
            problem = AllowedText(filter);
            return false;
        }

        const double assemblyMm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));
        const PartToAssembly transform(occurrence, assemblyMm);

        pick.point = transform.Point(pick.point);
        pick.start = transform.Point(pick.start);
        pick.end = transform.Point(pick.end);
        pick.direction = transform.Direction(pick.direction);

        // The clicked point is already in assembly coordinates.
        pick.clicked = (clicked != nullptr) ? ToVec3(clicked) * assemblyMm : pick.point;

        pick.occurrenceKey = TakeKey(occurrence->GetKey());
        pick.occurrenceName = ToUtf8(static_cast<const wchar_t*>(occurrence->GetName()));

        Log(L"[pick] on %s: point (%.4f, %.4f, %.4f) mm, direction (%.4f, %.4f, %.4f), clicked (%.4f, %.4f, %.4f) mm",
            FromUtf8(pick.occurrenceName).c_str(), pick.point.x, pick.point.y, pick.point.z, pick.direction.x,
            pick.direction.y, pick.direction.z, pick.clicked.x, pick.clicked.y, pick.clicked.z);
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not read the selection: hr=0x%08X", static_cast<unsigned>(error.Error()));
        pick = PickedGeometry();
        problem = L"That couldn't be read. " + std::wstring(AllowedText(filter));
        return false;
    }
}

bool ReadAssemblyBounds(IADSession* pAssembly, Vec3& lower, Vec3& upper)
{
    try
    {
        IADAssemblySessionPtr assembly = QI<IADAssemblySessionPtr>(pAssembly);
        if (assembly == nullptr)
            return false;

        IADPointPtr low, high;
        assembly->GetRootOccurrence()->GetExtents(&low, &high);
        if (low == nullptr || high == nullptr)
            return false;

        const double mm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));
        lower = ToVec3(low) * mm;
        upper = ToVec3(high) * mm;
        Log(L"[model] assembly extents (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f) mm", lower.x, lower.y, lower.z,
            upper.x, upper.y, upper.z);
        return upper.x >= lower.x && upper.y >= lower.y && upper.z >= lower.z;
    }
    catch (const _com_error& error)
    {
        Log(L"[model] could not read the assembly extents: hr=0x%08X", static_cast<unsigned>(error.Error()));
        return false;
    }
}

SelectionFilter SetSelectionFilter(IADSession* pAssembly, const SelectionFilter& wanted)
{
    SelectionFilter previous = wanted;
    try
    {
        IADDesignSelectionFilterPtr filter = QI<IADDesignSessionPtr>(pAssembly)->GetSelectionFilter();
        previous.solid = filter->GetSolid();
        previous.planes = filter->GetPlanes() != VARIANT_FALSE;
        previous.points = filter->GetPoints() != VARIANT_FALSE;

        if (previous.solid != wanted.solid)
            filter->PutSolid(wanted.solid);
        if (previous.planes != wanted.planes)
            filter->PutPlanes(wanted.planes ? VARIANT_TRUE : VARIANT_FALSE);
        if (previous.points != wanted.points)
            filter->PutPoints(wanted.points ? VARIANT_TRUE : VARIANT_FALSE);
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not set the selection filter: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
    return previous;
}

bool ReadSelectedLocate(IADSession* pAssembly, camcore::LocatedGeometry& located, std::wstring& problem)
{
    using Kind = camcore::LocatedGeometry::Kind;

    located = camcore::LocatedGeometry();
    problem.clear();
    const wchar_t* const allowed = L"Pick a design point or plane, a flat or cylindrical face, or a straight or "
                                   L"circular edge.";

    try
    {
        IADPointPtr clicked;
        IObjectCollectorPtr selected = pAssembly->SelectedObjectsEx(&clicked);
        if (selected == nullptr || selected->GetCount() == 0)
            return false;

        // Part items come as proxies. The assembly's own planes and points may not.
        IDispatchPtr item = selected->GetItem(_variant_t(0L));
        IADTargetProxyPtr proxy = QI<IADTargetProxyPtr>(item);
        IADOccurrencePtr occurrence = (proxy != nullptr) ? proxy->GetOccurrence() : nullptr;
        IDispatchPtr target = (proxy != nullptr) ? proxy->GetTarget() : item;
        if (target == nullptr)
        {
            problem = allowed;
            return false;
        }

        bool read = true;
        PickedGeometry pick;
        if (IADDesignPlanePtr plane = QI<IADDesignPlanePtr>(target))
        {
            IADPointPtr lower, upper, corner;
            plane->GetGeometry(&lower, &upper, &corner);
            located.kind = Kind::Plane;
            located.point = ToVec3(lower);
            located.direction = ToVec3(plane->GetNormal());
            Log(L"[locate] design plane %s", static_cast<const wchar_t*>(plane->GetName()));
        }
        else if (IADDesignPointPtr point = QI<IADDesignPointPtr>(target))
        {
            located.kind = Kind::Point;
            located.point = ToVec3(point->GetGeometry());
            Log(L"[locate] design point %s", static_cast<const wchar_t*>(point->GetName()));
        }
        else if (IADEdgePtr edge = QI<IADEdgePtr>(target))
        {
            read = ReadEdge(edge, pick);
        }
        else if (IADFacePtr face = QI<IADFacePtr>(target))
        {
            read = ReadFace(face, pick);
        }
        else
        {
            read = false;
        }

        if (!read)
        {
            problem = allowed;
            return false;
        }

        switch (pick.kind)
        {
        case PickKind::PlanarFace:      located.kind = Kind::Plane; break;
        case PickKind::CylindricalFace: located.kind = Kind::Line; break;
        case PickKind::LineEdge:        located.kind = Kind::Line; break;
        case PickKind::CircleEdge:      located.kind = Kind::Point; break;
        default:                        break;
        }
        if (pick.kind != PickKind::None)
        {
            located.point = pick.point;
            located.direction = pick.direction;
        }

        const double assemblyMm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));
        const PartToAssembly transform(occurrence, assemblyMm);
        located.point = transform.Point(located.point);
        located.direction = transform.Direction(located.direction);

        Log(L"[locate] %s: point (%.4f, %.4f, %.4f) mm, direction (%.4f, %.4f, %.4f)",
            occurrence != nullptr ? static_cast<const wchar_t*>(occurrence->GetName()) : L"assembly",
            located.point.x, located.point.y, located.point.z, located.direction.x, located.direction.y,
            located.direction.z);
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[locate] could not read the selection: hr=0x%08X", static_cast<unsigned>(error.Error()));
        problem = std::wstring(L"That couldn't be read. ") + allowed;
        return false;
    }
}

void ClearSelection(IADSession* pAssembly)
{
    try
    {
        pAssembly->Select(theApp.m_pRoot->NewObjectCollector());
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not clear the selection: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
}
