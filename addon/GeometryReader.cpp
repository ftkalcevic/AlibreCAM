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

        double Length(double part) const
        {
            return part * m_partToAssembly * m_assemblyMm;
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

    // A hole's ends are the centres of the circles bounding its face, furthest
    // apart along the axis. Without two, both ends are the base point.
    void FindCylinderEnds(IADFace* pFace, PickedGeometry& pick)
    {
        pick.start = pick.end = pick.point;

        Vec3 axis;
        if (!camcore::Normalise(pick.direction, axis))
            return;

        bool found = false;
        double low = 0.0, high = 0.0;
        IADEdgesPtr edges = pFace->GetEdges();
        const long count = (edges != nullptr) ? edges->GetCount() : 0;
        for (long i = 0; i < count; ++i)
        {
            IADCurvePtr curve = edges->GetItem(i)->GetGeometry();
            Vec3 centre;
            if (IADCirclePtr circle = QI<IADCirclePtr>(curve))
                centre = ToVec3(circle->GetCenter());
            else if (IADCircularArcPtr arc = QI<IADCircularArcPtr>(curve))
                centre = ToVec3(arc->GetCenter());
            else
                continue;

            const double along = camcore::Dot(centre - pick.point, axis);
            if (!found || along < low)
            {
                low = along;
                pick.start = centre;
            }
            if (!found || along > high)
            {
                high = along;
                pick.end = centre;
            }
            found = true;
        }
        Log(L"[pick] cylinder ends %s, %.4f apart", found ? L"found" : L"not found", high - low);
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
            pick.radius = cylinder->GetRadius();
            FindCylinderEnds(pFace, pick);
            Log(L"[pick] cylindrical face, radius %g", pick.radius);
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

    // Lines, circles and arcs. With allowCurves, any other curve too, such as a
    // spline, as its ends and the point halfway along its parameter.
    bool ReadEdge(IADEdge* pEdge, PickedGeometry& pick, bool allowCurves = false)
    {
        IADCurvePtr curve = pEdge->GetGeometry();
        if (curve == nullptr)
            return false;

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
            pick.radius = circle->GetRadius();
        }
        else if (IADCircularArcPtr arc = QI<IADCircularArcPtr>(curve))
        {
            pick.kind = PickKind::CircleEdge;
            pick.point = ToVec3(arc->GetCenter());
            pick.direction = ToVec3(arc->GetAxis());
            pick.radius = arc->GetRadius();
        }
        else if (allowCurves)
        {
            double low = 0.0, high = 0.0;
            curve->GetParameterExtents(&low, &high);
            IADVertexPtr startVertex = pEdge->GetStartVertex();
            IADVertexPtr endVertex = pEdge->GetEndVertex();
            double from = low, to = high;
            if (startVertex != nullptr && endVertex != nullptr)
            {
                from = curve->ParamAtPoint(startVertex->GetPoint());
                to = curve->ParamAtPoint(endVertex->GetPoint());
            }

            pick.kind = PickKind::CurveEdge;
            pick.start = ToVec3(curve->PointAtParam(from));
            pick.end = ToVec3(curve->PointAtParam(to));
            pick.point = ToVec3(curve->PointAtParam((from + to) / 2.0));
            pick.direction = pick.end - pick.start;
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
        switch (filter)
        {
        case PickFilter::OriginPosition: return L"Pick a straight or circular edge, or a design point.";
        case PickFilter::OriginX:        return L"Pick a straight edge or a design axis.";
        default:
            return L"Pick a flat or cylindrical face, a straight or circular edge, or a design plane or axis.";
        }
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

        // Part items come as proxies. The assembly's own planes, axes and points may not.
        IDispatchPtr item = selected->GetItem(_variant_t(0L));
        IADTargetProxyPtr proxy = QI<IADTargetProxyPtr>(item);
        IADOccurrencePtr occurrence = (proxy != nullptr) ? proxy->GetOccurrence() : nullptr;
        IDispatchPtr target = (proxy != nullptr) ? proxy->GetTarget() : item;
        if (target == nullptr)
        {
            problem = AllowedText(filter);
            return false;
        }

        bool read = false;
        bool design = false;
        if (IADDesignPlanePtr plane = QI<IADDesignPlanePtr>(target))
        {
            if (filter == PickFilter::OriginPlane)
            {
                IADPointPtr lower, upper, corner;
                plane->GetGeometry(&lower, &upper, &corner);
                pick.kind = PickKind::DesignPlane;
                pick.point = ToVec3(lower);
                pick.direction = ToVec3(plane->GetNormal());
                pick.targetName = ToUtf8(static_cast<const wchar_t*>(plane->GetName()));
                read = design = true;
            }
        }
        else if (IADDesignAxisPtr axis = QI<IADDesignAxisPtr>(target))
        {
            if (filter == PickFilter::OriginPlane || filter == PickFilter::OriginX)
            {
                IADPointPtr first, second;
                axis->GetGeometry(&first, &second);
                pick.kind = PickKind::DesignAxis;
                pick.point = (first != nullptr) ? ToVec3(first) : Vec3();
                pick.direction = ToVec3(axis->GetDirection());
                pick.targetName = ToUtf8(static_cast<const wchar_t*>(axis->GetName()));
                read = design = true;
            }
        }
        else if (IADDesignPointPtr point = QI<IADDesignPointPtr>(target))
        {
            if (filter == PickFilter::OriginPosition)
            {
                pick.kind = PickKind::DesignPoint;
                pick.point = ToVec3(point->GetGeometry());
                pick.targetName = ToUtf8(static_cast<const wchar_t*>(point->GetName()));
                read = design = true;
            }
        }
        else if (occurrence == nullptr)
        {
            read = false;       // a face or edge must be on a part
        }
        else if (IADEdgePtr edge = QI<IADEdgePtr>(target))
        {
            read = ReadEdge(edge, pick) && (filter != PickFilter::OriginX || pick.kind == PickKind::LineEdge);
        }
        else if (IADFacePtr face = QI<IADFacePtr>(target))
        {
            read = (filter == PickFilter::OriginPlane) && ReadFace(face, pick);
        }

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
        pick.radius = transform.Length(pick.radius);

        // The clicked point is already in assembly coordinates.
        pick.clicked = (clicked != nullptr && !design) ? ToVec3(clicked) * assemblyMm : pick.point;

        if (occurrence != nullptr)
        {
            pick.occurrenceKey = TakeKey(occurrence->GetKey());
            pick.occurrenceName = ToUtf8(static_cast<const wchar_t*>(occurrence->GetName()));
        }
        if (design)
            Log(L"[pick] design item %s", FromUtf8(pick.targetName).c_str());

        Log(L"[pick] on %s: point (%.4f, %.4f, %.4f) mm, direction (%.4f, %.4f, %.4f), clicked (%.4f, %.4f, %.4f) mm",
            pick.occurrenceName.empty() ? L"the assembly" : FromUtf8(pick.occurrenceName).c_str(), pick.point.x, pick.point.y, pick.point.z, pick.direction.x,
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
        previous.axes = filter->GetAxes() != VARIANT_FALSE;
        previous.points = filter->GetPoints() != VARIANT_FALSE;

        if (previous.solid != wanted.solid)
            filter->PutSolid(wanted.solid);
        if (previous.planes != wanted.planes)
            filter->PutPlanes(wanted.planes ? VARIANT_TRUE : VARIANT_FALSE);
        if (previous.axes != wanted.axes)
            filter->PutAxes(wanted.axes ? VARIANT_TRUE : VARIANT_FALSE);
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

namespace
{
    // Reads one selected item as a feature. Returns false if it isn't one the filter allows.
    bool ReadFeatureItem(IDispatch* pItem, double assemblyMm, camcore::FeatureFilter filter, PickedGeometry& pick)
    {
        pick = PickedGeometry();

        // Part items come as proxies. The assembly's own design points may not.
        IADTargetProxyPtr proxy = QI<IADTargetProxyPtr>(pItem);
        IADOccurrencePtr occurrence = (proxy != nullptr) ? proxy->GetOccurrence() : nullptr;
        IDispatchPtr target = (proxy != nullptr) ? proxy->GetTarget() : IDispatchPtr(pItem);
        if (target == nullptr)
            return false;

        bool read = false;
        if (IADDesignPointPtr point = QI<IADDesignPointPtr>(target))
        {
            pick.kind = PickKind::DesignPoint;
            pick.point = ToVec3(point->GetGeometry());
            pick.targetName = ToUtf8(static_cast<const wchar_t*>(point->GetName()));
            read = true;
        }
        else if (IADEdgePtr edge = QI<IADEdgePtr>(target))
        {
            read = ReadEdge(edge, pick, true);
        }
        else if (IADFacePtr face = QI<IADFacePtr>(target))
        {
            read = ReadFace(face, pick);
        }

        // Faces and edges need their occurrence to be found again.
        if (!read || !camcore::FeatureAllowed(filter, pick.kind) ||
            (occurrence == nullptr && pick.kind != PickKind::DesignPoint))
            return false;

        const PartToAssembly transform(occurrence, assemblyMm);
        pick.point = transform.Point(pick.point);
        pick.start = transform.Point(pick.start);
        pick.end = transform.Point(pick.end);
        pick.direction = transform.Direction(pick.direction);
        pick.radius = transform.Length(pick.radius);
        pick.clicked = pick.point;

        if (occurrence != nullptr)
        {
            pick.occurrenceKey = TakeKey(occurrence->GetKey());
            pick.occurrenceName = ToUtf8(static_cast<const wchar_t*>(occurrence->GetName()));
        }
        return true;
    }

    // A saved key as the array BindKeyToItem takes. The caller destroys it.
    SAFEARRAY* KeyArray(const camcore::PersistentKey& key)
    {
        if (key.IsEmpty())
            return nullptr;

        const VARTYPE vt = static_cast<VARTYPE>(key.elementType);
        SAFEARRAY* pProbe = SafeArrayCreateVector(vt, 0, 1);
        if (pProbe == nullptr)
            return nullptr;
        const UINT elementSize = SafeArrayGetElemsize(pProbe);
        SafeArrayDestroy(pProbe);
        if (elementSize == 0)
            return nullptr;

        const size_t bytes = key.hex.size() / 2;
        SAFEARRAY* pArray = SafeArrayCreateVector(vt, 0, static_cast<ULONG>(bytes / elementSize));
        BYTE* pData = nullptr;
        if (pArray != nullptr && SUCCEEDED(SafeArrayAccessData(pArray, reinterpret_cast<void**>(&pData))))
        {
            const auto nibble = [](char c) -> BYTE
            {
                return static_cast<BYTE>(c >= 'A' ? c - 'A' + 10 : c - '0');
            };
            for (size_t i = 0; i < bytes; ++i)
                pData[i] = static_cast<BYTE>((nibble(key.hex[i * 2]) << 4) | nibble(key.hex[i * 2 + 1]));
            SafeArrayUnaccessData(pArray);
        }
        return pArray;
    }

    IDispatchPtr BindKey(IADSession* pSession, const camcore::PersistentKey& key, ADObjectType type)
    {
        SAFEARRAY* pArray = KeyArray(key);
        if (pArray == nullptr)
            return nullptr;

        IDispatchPtr result;
        try
        {
            result = pSession->BindKeyToItem(&pArray, type);
        }
        catch (const _com_error&)
        {
            // An item that's been deleted no longer binds.
        }
        SafeArrayDestroy(pArray);
        return result;
    }
}

bool ReadSelectedFeatures(IADSession* pAssembly, camcore::FeatureFilter filter, std::vector<PickedGeometry>& features,
                          int& ignored)
{
    features.clear();
    ignored = 0;

    try
    {
        IADPointPtr clicked;
        IObjectCollectorPtr selected = pAssembly->SelectedObjectsEx(&clicked);
        const long count = (selected != nullptr) ? selected->GetCount() : 0;
        const double assemblyMm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));

        for (long i = 0; i < count; ++i)
        {
            PickedGeometry pick;
            bool read = false;
            try
            {
                read = ReadFeatureItem(selected->GetItem(_variant_t(i)), assemblyMm, filter, pick);
            }
            catch (const _com_error& error)
            {
                Log(L"[features] could not read selected item %ld: hr=0x%08X", i, static_cast<unsigned>(error.Error()));
            }

            if (read)
                features.push_back(pick);
            else
                ++ignored;
        }
        Log(L"[features] %ld selected: %zu features, %d ignored", count, features.size(), ignored);
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not read the selection: hr=0x%08X", static_cast<unsigned>(error.Error()));
        return false;
    }
}

namespace
{
    // The feature's occurrence and face or edge, found again by their keys.
    // Design points have no key to find them by.
    bool BindFeature(IADSession* pAssembly, const PickedGeometry& feature, IADOccurrencePtr& occurrence,
                     IDispatchPtr& target)
    {
        if (feature.kind == PickKind::DesignPoint)
            return false;

        occurrence = QI<IADOccurrencePtr>(BindKey(pAssembly, feature.occurrenceKey, ADObjectType_AD_OCCURRENCE));
        if (occurrence == nullptr)
            return false;

        // Face and edge keys belong to the part.
        IADSessionPtr part = QI<IADSessionPtr>(occurrence->GetDesignSession());
        target = (part != nullptr) ? BindKey(part, feature.targetKey, ADObjectType_AD_TOPOLOGY) : nullptr;
        return target != nullptr;
    }

    // The feature as something Alibre can select, or nothing if it can't be found.
    IADTargetProxyPtr FindFeature(IADSession* pAssembly, const PickedGeometry& feature)
    {
        IADOccurrencePtr occurrence;
        IDispatchPtr target;
        if (!BindFeature(pAssembly, feature, occurrence, target))
            return nullptr;
        return pAssembly->NewTargetProxy(occurrence, target);
    }

    // Curved edges are sampled with this many segments over a full turn, and
    // proportionally fewer for shorter arcs, but never fewer than this.
    const int kSegmentsPerTurn = 72;
    const int kMinimumCurveSegments = 4;
    const double kTwoPi = 6.28318530717958647692;

    // An edge as points in part coordinates, from its start vertex to its end.
    // Lines are their two ends. Curves are sampled along the curve.
    bool SampleEdge(IADEdge* pEdge, std::vector<Vec3>& points)
    {
        points.clear();
        IADVertexPtr startVertex = pEdge->GetStartVertex();
        IADVertexPtr endVertex = pEdge->GetEndVertex();
        IADCurvePtr curve = pEdge->GetGeometry();
        if (curve == nullptr)
            return false;

        if (QI<IADLinePtr>(curve) != nullptr)
        {
            if (startVertex == nullptr || endVertex == nullptr)
                return false;
            points.push_back(ToVec3(startVertex->GetPoint()));
            points.push_back(ToVec3(endVertex->GetPoint()));
            return true;
        }

        double low = 0.0, high = 0.0;
        curve->GetParameterExtents(&low, &high);

        // A circle or arc goes round, so its parameter wraps after a full turn.
        const bool round = QI<IADCirclePtr>(curve) != nullptr || QI<IADCircularArcPtr>(curve) != nullptr;
        const double period = high - low;

        double from = low, to = high;
        const bool closed = startVertex == nullptr || endVertex == nullptr ||
                            camcore::Length(ToVec3(startVertex->GetPoint()) - ToVec3(endVertex->GetPoint())) < 1e-9;
        if (!closed)
        {
            from = curve->ParamAtPoint(startVertex->GetPoint());
            to = curve->ParamAtPoint(endVertex->GetPoint());
            if (round && to <= from)
                to += period;
        }

        const double span = std::fabs(to - from);
        const double turns = (round && period > 0.0) ? span / period : 1.0;
        const int segments = (std::max)(kMinimumCurveSegments, static_cast<int>(std::ceil(turns * kSegmentsPerTurn)));
        for (int i = 0; i <= segments; ++i)
        {
            double t = from + (to - from) * i / segments;
            if (round && period > 0.0 && t > high)
                t = low + std::fmod(t - low, period);
            points.push_back(ToVec3(curve->PointAtParam(t)));
        }
        return true;
    }
}

namespace
{
    // A circle or arc edge as arcs in assembly mm, from its start vertex to its
    // end, or round once from where its parameter starts if it's closed, in two
    // halves. Each arc's axis is turned so the arc runs anticlockwise about it,
    // judged from a point halfway along, so neither Alibre's parameter direction
    // nor a mirrored placement matters. False if it isn't a circle or arc.
    bool ReadRoundEdge(IADEdge* pEdge, const PartToAssembly& transform, camcore::Path& piece)
    {
        IADCurvePtr curve = pEdge->GetGeometry();
        Vec3 centre, axis;
        if (IADCirclePtr circle = QI<IADCirclePtr>(curve))
        {
            centre = ToVec3(circle->GetCenter());
            axis = ToVec3(circle->GetAxis());
        }
        else if (IADCircularArcPtr arc = QI<IADCircularArcPtr>(curve))
        {
            centre = ToVec3(arc->GetCenter());
            axis = ToVec3(arc->GetAxis());
        }
        else
        {
            return false;
        }

        double low = 0.0, high = 0.0;
        curve->GetParameterExtents(&low, &high);
        const double period = high - low;
        auto at = [&](double t) {
            if (period > 0.0 && t > high)
                t = low + std::fmod(t - low, period);
            return transform.Point(ToVec3(curve->PointAtParam(t)));
        };

        IADVertexPtr startVertex = pEdge->GetStartVertex();
        IADVertexPtr endVertex = pEdge->GetEndVertex();
        const bool closed = startVertex == nullptr || endVertex == nullptr ||
                            camcore::Length(ToVec3(startVertex->GetPoint()) - ToVec3(endVertex->GetPoint())) < 1e-9;

        std::vector<double> params;
        if (closed)
        {
            params = { low, low + period / 2.0, high };
        }
        else
        {
            const double from = curve->ParamAtPoint(startVertex->GetPoint());
            double to = curve->ParamAtPoint(endVertex->GetPoint());
            if (to <= from)
                to += period;
            params = { from, to };
        }

        const Vec3 worldCentre = transform.Point(centre);
        const Vec3 worldAxis = transform.Direction(axis);
        piece.clear();
        for (size_t i = 0; i < params.size(); ++i)
        {
            camcore::PathVertex vertex;
            vertex.point = at(params[i]);
            if (i + 1 < params.size())
            {
                const Vec3 middle = at((params[i] + params[i + 1]) / 2.0);
                const double turn = camcore::Dot(camcore::Cross(vertex.point - worldCentre, middle - worldCentre), worldAxis);
                vertex.arc = true;
                vertex.centre = worldCentre;
                vertex.axis = (turn < 0.0) ? worldAxis * -1.0 : worldAxis;
            }
            piece.push_back(vertex);
        }

        // The ends are exactly the vertices, where there are any.
        if (!closed)
        {
            piece.front().point = transform.Point(ToVec3(startVertex->GetPoint()));
            piece.back().point = transform.Point(ToVec3(endVertex->GetPoint()));
        }
        return true;
    }
}

bool ReadFeatureOutline(IADSession* pAssembly, const PickedGeometry& feature, std::vector<camcore::Path>& pieces,
                        std::wstring& problem)
{
    pieces.clear();
    problem.clear();

    try
    {
        IADOccurrencePtr occurrence;
        IDispatchPtr target;
        if (!BindFeature(pAssembly, feature, occurrence, target))
        {
            problem = L"A feature can't be found in the model. It may have been deleted.";
            return false;
        }

        // A face's outline is its outer loop's edges. An edge is itself.
        std::vector<IADEdgePtr> edges;
        if (IADFacePtr face = QI<IADFacePtr>(target))
        {
            IADLoopsPtr loops = face->GetLoops();
            const long loopCount = (loops != nullptr) ? loops->GetCount() : 0;
            for (long i = 0; i < loopCount; ++i)
            {
                IADLoopPtr loop = loops->GetItem(i);
                if (loop->GetIsOuter() == VARIANT_FALSE)
                    continue;
                IADEdgesPtr loopEdges = loop->GetEdges();
                const long edgeCount = (loopEdges != nullptr) ? loopEdges->GetCount() : 0;
                for (long e = 0; e < edgeCount; ++e)
                    edges.push_back(loopEdges->GetItem(e));
            }
            if (edges.empty())
            {
                problem = L"The face has no outer edges.";
                return false;
            }
        }
        else if (IADEdgePtr edge = QI<IADEdgePtr>(target))
        {
            edges.push_back(edge);
        }
        else
        {
            problem = L"A feature isn't a face or an edge.";
            return false;
        }

        const double assemblyMm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));
        const PartToAssembly transform(occurrence, assemblyMm);
        size_t arcs = 0;
        for (const IADEdgePtr& edge : edges)
        {
            // Circles and arcs stay arcs. Lines are their ends, and other curves are sampled.
            camcore::Path piece;
            if (ReadRoundEdge(edge, transform, piece))
            {
                ++arcs;
                pieces.push_back(std::move(piece));
                continue;
            }

            std::vector<Vec3> points;
            if (!SampleEdge(edge, points))
            {
                problem = L"An edge's shape couldn't be read.";
                return false;
            }
            for (Vec3& point : points)
                point = transform.Point(point);
            pieces.push_back(camcore::PathFromPoints(points));
        }
        Log(L"[outline] %s: %zu edges, %zu of them circles or arcs", FromUtf8(feature.occurrenceName).c_str(),
            pieces.size(), arcs);
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[outline] could not read a feature: hr=0x%08X", static_cast<unsigned>(error.Error()));
        problem = L"A feature's outline couldn't be read.";
        return false;
    }
}

int SelectFeatures(IADSession* pAssembly, const std::vector<PickedGeometry>& features)
{
    int selected = 0;
    try
    {
        IObjectCollectorPtr collector = theApp.m_pRoot->NewObjectCollector();
        for (const PickedGeometry& feature : features)
        {
            if (IADTargetProxyPtr proxy = FindFeature(pAssembly, feature))
            {
                collector->Add(proxy);
                ++selected;
            }
        }

        pAssembly->Select(collector);
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not select the features: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    Log(L"[features] selected %d of %zu features", selected, features.size());
    return selected;
}

void HighlightFeature(IADSession* pAssembly, const PickedGeometry* pFeature)
{
    try
    {
        IADTargetProxyPtr proxy = (pFeature != nullptr) ? FindFeature(pAssembly, *pFeature) : nullptr;
        pAssembly->Highlight(proxy);
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not highlight a feature: hr=0x%08X", static_cast<unsigned>(error.Error()));
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

namespace
{
    // The part behind a selected item: an occurrence itself, or a face or edge's.
    IADOccurrencePtr SelectedOccurrence(IDispatch* pItem)
    {
        if (IADOccurrencePtr occurrence = QI<IADOccurrencePtr>(pItem))
            return occurrence;
        IADTargetProxyPtr proxy = QI<IADTargetProxyPtr>(pItem);
        if (proxy == nullptr)
            return nullptr;
        if (IADOccurrencePtr occurrence = proxy->GetOccurrence())
            return occurrence;
        return QI<IADOccurrencePtr>(proxy->GetTarget());
    }
}

bool ReadSelectedParts(IADSession* pAssembly, std::vector<camcore::StockPart>& parts)
{
    parts.clear();
    try
    {
        IADPointPtr clicked;
        IObjectCollectorPtr selected = pAssembly->SelectedObjectsEx(&clicked);
        const long count = (selected != nullptr) ? selected->GetCount() : 0;
        for (long i = 0; i < count; ++i)
        {
            IADOccurrencePtr occurrence = SelectedOccurrence(selected->GetItem(_variant_t(i)));
            if (occurrence == nullptr)
            {
                Log(L"[stock] selected item %ld has no part", i);
                continue;
            }

            camcore::StockPart part;
            part.occurrenceKey = TakeKey(occurrence->GetKey());
            part.name = ToUtf8(static_cast<const wchar_t*>(occurrence->GetName()));
            const bool listed = std::any_of(parts.begin(), parts.end(), [&](const camcore::StockPart& other)
                                            { return other.occurrenceKey.hex == part.occurrenceKey.hex; });
            if (!listed && !part.occurrenceKey.IsEmpty())
                parts.push_back(part);
        }
        Log(L"[stock] %ld selected items give %zu parts", count, parts.size());
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[stock] could not read the selected parts: hr=0x%08X", static_cast<unsigned>(error.Error()));
        return false;
    }
}

bool ReadPartsBounds(IADSession* pAssembly, const std::vector<camcore::StockPart>& parts, Vec3& lower, Vec3& upper,
                     std::wstring& problem)
{
    problem.clear();
    if (parts.empty())
    {
        if (ReadAssemblyBounds(pAssembly, lower, upper))
            return true;
        problem = L"The assembly's size couldn't be read.";
        return false;
    }

    try
    {
        const double mm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(pAssembly));
        bool first = true;
        for (const camcore::StockPart& part : parts)
        {
            IADOccurrencePtr occurrence =
                QI<IADOccurrencePtr>(BindKey(pAssembly, part.occurrenceKey, ADObjectType_AD_OCCURRENCE));
            if (occurrence == nullptr)
            {
                problem = L"The part " + FromUtf8(part.name) + L" can't be found. It may have been deleted.";
                return false;
            }

            // Read in the assembly's coordinates, as the whole assembly's extents are.
            IADPointPtr low, high;
            occurrence->GetExtents(&low, &high);
            if (low == nullptr || high == nullptr)
            {
                problem = L"The size of " + FromUtf8(part.name) + L" couldn't be read.";
                return false;
            }
            const Vec3 a = ToVec3(low) * mm;
            const Vec3 b = ToVec3(high) * mm;
            Log(L"[stock] %s extents (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f) mm", FromUtf8(part.name).c_str(), a.x,
                a.y, a.z, b.x, b.y, b.z);
            if (first)
            {
                lower = a;
                upper = b;
                first = false;
            }
            else
            {
                lower = { (std::min)(lower.x, a.x), (std::min)(lower.y, a.y), (std::min)(lower.z, a.z) };
                upper = { (std::max)(upper.x, b.x), (std::max)(upper.y, b.y), (std::max)(upper.z, b.z) };
            }
        }
        return true;
    }
    catch (const _com_error& error)
    {
        Log(L"[stock] could not read the parts' extents: hr=0x%08X", static_cast<unsigned>(error.Error()));
        problem = L"The parts' size couldn't be read.";
        return false;
    }
}

bool ReadPartFacets(const std::wstring& path, std::vector<Vec3>& triangles, std::wstring& problem)
{
    triangles.clear();
    problem.clear();
    if (theApp.m_pRoot == nullptr)
    {
        problem = L"Alibre isn't available.";
        return false;
    }

    IADSessionPtr session;
    try
    {
        const ULONGLONG start = GetTickCount64();
        session = theApp.m_pRoot->OpenFileEx(_bstr_t(path.c_str()), VARIANT_FALSE);
        if (session == nullptr)
        {
            problem = L"The stock part " + path + L" couldn't be opened.";
            return false;
        }

        IADPartSessionPtr part = QI<IADPartSessionPtr>(session);
        if (part == nullptr)
        {
            problem = L"The stock file " + path + L" isn't a part.";
        }
        else
        {
            const double mm = MillimetresPerModelUnit(QI<IADDesignSessionPtr>(session));
            SAFEARRAY* pFacets = part->GetFacetData();
            if (pFacets != nullptr)
            {
                VARTYPE vt = VT_EMPTY;
                SafeArrayGetVartype(pFacets, &vt);
                LONG lower = 0, upper = -1;
                SafeArrayGetLBound(pFacets, 1, &lower);
                SafeArrayGetUBound(pFacets, 1, &upper);
                const LONG count = upper - lower + 1;
                double* pData = nullptr;
                if (vt == VT_R8 && count >= 9 &&
                    SUCCEEDED(SafeArrayAccessData(pFacets, reinterpret_cast<void**>(&pData))))
                {
                    triangles.reserve(static_cast<size_t>(count / 3));
                    for (LONG i = 0; i + 8 < count; i += 9)
                    {
                        for (int k = 0; k < 9; k += 3)
                            triangles.push_back(Vec3{ pData[i + k], pData[i + k + 1], pData[i + k + 2] } * mm);
                    }
                    SafeArrayUnaccessData(pFacets);
                }
                SafeArrayDestroy(pFacets);
            }
            if (triangles.empty())
                problem = L"The stock part " + path + L" has no solid to read.";
            Log(L"[stock] read %zu triangles from %s in %llu ms", triangles.size() / 3, path.c_str(),
                GetTickCount64() - start);
        }
        session->Close(VARIANT_FALSE);
    }
    catch (const _com_error& error)
    {
        Log(L"[stock] could not read %s: hr=0x%08X", path.c_str(), static_cast<unsigned>(error.Error()));
        problem = L"The stock part " + path + L" couldn't be read.";
        try
        {
            if (session != nullptr)
                session->Close(VARIANT_FALSE);
        }
        catch (const _com_error&)
        {
        }
        triangles.clear();
        return false;
    }
    return problem.empty();
}
