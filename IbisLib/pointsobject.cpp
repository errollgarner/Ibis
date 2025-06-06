/*=========================================================================
Ibis Neuronav
Copyright (c) Simon Drouin, Anna Kochanowska, Louis Collins.
All rights reserved.
See Copyright.txt or http://ibisneuronav.org/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/
#include "pointsobject.h"

#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkMath.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkSmartPointer.h>
#include <vtkTransform.h>

#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>

#include "application.h"
#include "ibisconfig.h"
#include "pointcolorwidget.h"
#include "pointerobject.h"
#include "pointpropertieswidget.h"
#include "pointrepresentation.h"
#include "polydataobject.h"
#include "scenemanager.h"
#include "view.h"
#include "vtkTagWriter.h"

ObjectSerializationMacro( PointsObject );

const int PointsObject::InvalidPointIndex = -1;
const int PointsObject::MinRadius         = 1;
const int PointsObject::MaxRadius         = 16;
const int PointsObject::MinLabelSize      = 6;
const int PointsObject::MaxLabelSize      = 16;

PointsObject::PointsObject() : SceneObject()
{
    m_pointCoordinates   = vtkSmartPointer<vtkPoints>::New();
    m_selectedPointIndex = InvalidPointIndex;
    m_pointRadius3D      = 2.0;
    m_pointRadius2D      = 20.0;
    m_labelSize          = 8.0;
    m_activeColor[0]     = 1.0;
    m_activeColor[1]     = 0.7;
    m_activeColor[2]     = 0.0;
    m_inactiveColor[0]   = 0.7;
    m_inactiveColor[1]   = 0.7;
    m_inactiveColor[2]   = 0.6;
    m_selectedColor[0]   = 0.0;
    m_selectedColor[1]   = 1.0;
    m_selectedColor[2]   = 0.0;
    m_opacity            = 1.0;
    m_movingPointIndex   = PointsObject::InvalidPointIndex;
    m_picker             = vtkSmartPointer<vtkCellPicker>::New();
    m_pickable           = false;
    m_pickabilityLocked  = false;
    m_showLabels         = true;
    m_computeDistance    = false;
    m_lineToPointerTip   = 0;
    for( int i = 0; i < 3; i++ ) m_lineToPointerColor[i] = 1.0;
}

PointsObject::~PointsObject() {}

void PointsObject::Serialize( Serializer * ser )
{
    SceneObject::Serialize( ser );

    ::Serialize( ser, "PointRadius3D", m_pointRadius3D );
    ::Serialize( ser, "PointRadius2D", m_pointRadius2D );
    ::Serialize( ser, "LabelSize", m_labelSize );
    ::Serialize( ser, "EnabledColor", m_activeColor, 3 );
    ::Serialize( ser, "DisabledColor", m_inactiveColor, 3 );
    ::Serialize( ser, "SelectededColor", m_selectedColor, 3 );
    ::Serialize( ser, "LineToPointerColor", m_lineToPointerColor, 3 );
    ::Serialize( ser, "Opacity", m_opacity );
    ::Serialize( ser, "SelectedPointIndex", m_selectedPointIndex );
    int numberOfPoints;
    double coords[3];
    QString pointName, timeStamp( "n/a" );
    if( !ser->IsReader() )
    {
        numberOfPoints = m_pointCoordinates->GetNumberOfPoints();
        ::Serialize( ser, "NumberOfPoints", numberOfPoints );
        for( int i = 0; i < numberOfPoints; i++ )
        {
            pointName = m_pointNames.at( i );
            timeStamp = m_timeStamps.at( i );
            m_pointCoordinates->GetPoint( i, coords );
            QString sectionName = QString( "Point_%1" ).arg( i );
            ser->BeginSection( sectionName.toUtf8().data() );
            ::Serialize( ser, "PointName", pointName );
            ::Serialize( ser, "PointCoordinates", coords, 3 );
            ::Serialize( ser, "PointTimeStamp", timeStamp );
            ser->EndSection();
        }
    }
    else
    {
        this->SetEnabledColor( m_activeColor );
        this->SetDisabledColor( m_inactiveColor );
        this->SetSelectedColor( m_selectedColor );
        ::Serialize( ser, "NumberOfPoints", numberOfPoints );
        m_pointNames.clear();
        m_timeStamps.clear();
        m_pointCoordinates->Reset();
        for( int i = 0; i < numberOfPoints; i++ )
        {
            QString sectionName = QString( "Point_%1" ).arg( i );
            ser->BeginSection( sectionName.toUtf8().data() );
            ::Serialize( ser, "PointName", pointName );
            ::Serialize( ser, "PointCoordinates", coords, 3 );
            ::Serialize( ser, "PointTimeStamp", timeStamp );
            ser->EndSection();
            this->AddPointLocal( coords, pointName, timeStamp );
        }
        if( numberOfPoints > 0 && m_selectedPointIndex == InvalidPointIndex ) m_selectedPointIndex = 0;

        if( m_selectedPointIndex != InvalidPointIndex ) this->SetSelectedPoint( m_selectedPointIndex );
    }
}

void PointsObject::Export()
{
    Q_ASSERT( this->GetManager() );

    if( m_pointCoordinates->GetNumberOfPoints() > 0 )
    {
        QString workingDirectory( this->GetManager()->GetSceneDirectory() );
        if( !QFile::exists( workingDirectory ) )
        {
            workingDirectory = QDir::homePath();
        }
        QString name( this->Name );
        name.append( ".tag" );
        QString fullName( workingDirectory );
        fullName.append( "/" );
        fullName.append( name );
        QString saveName = Application::GetInstance().GetFileNameSave( tr( "Save Object" ), fullName, tr( "*.tag" ) );
        if( saveName.isEmpty() ) return;
        if( QFile::exists( saveName ) )
        {
            int ret = QMessageBox::warning( 0, tr( "Save Points" ), saveName, QMessageBox::Yes | 
                                            QMessageBox::No, QMessageBox::No );
            if( ret == QMessageBox::No ) return;
        }
        QFileInfo info( saveName );
        this->SetDataFileName( name );
        std::vector<std::string> pointNames;
        std::vector<std::string> timeStamps;
        for( int i = 0; i < m_pointCoordinates->GetNumberOfPoints(); i++ )
        {
            pointNames.push_back( m_pointNames.at( i ).toUtf8().data() );
            timeStamps.push_back( m_timeStamps.at( i ).toUtf8().data() );
        }
        vtkTagWriter * writer = vtkTagWriter::New();
        writer->SetFileName( info.absoluteFilePath().toUtf8().data() );
        writer->SetPointNames( pointNames );
        writer->AddVolume( m_pointCoordinates, this->Name.toUtf8().data() );
        writer->SetTimeStamps( timeStamps );
        writer->Write();
        writer->Delete();
    }
    else
        QMessageBox::warning( 0, tr( "Error: " ), tr( "There are no points to save." ), QMessageBox::Ok );
}

void PointsObject::Setup( View * view )
{
    // register to receive mouse interaction event from this view
    view->AddInteractionObject( this, 0.5 );

    SceneObject::Setup( view );
}

void PointsObject::Release( View * view )
{
    view->RemoveInteractionObject( this );
    SceneObject::Release( view );
}

void PointsObject::Hide()
{
    PointList::iterator it = m_pointList.begin();
    for( ; it != m_pointList.end(); ++it )
    {
        ( *it )->SetHidden( true );
    }
    emit ObjectModified();
}

void PointsObject::Show()
{
    PointList::iterator it = m_pointList.begin();
    for( ; it != m_pointList.end(); ++it )
    {
        ( *it )->SetHidden( false );
    }
    this->UpdatePointsVisibility();
    emit ObjectModified();
}

void PointsObject::CreateSettingsWidgets( QWidget * parent, QVector<QWidget *> * widgets )
{
    PointPropertiesWidget * props = new PointPropertiesWidget( parent );
    props->setAttribute( Qt::WA_DeleteOnClose );
    props->SetPointsObject( this );
    props->setObjectName( "Properties" );
    widgets->append( props );
    PointColorWidget * color = new PointColorWidget( parent );
    color->setAttribute( Qt::WA_DeleteOnClose );
    color->SetPointsObject( this );
    color->setObjectName( "Color" );
    widgets->append( color );
}

void PointsObject::AddPoint( const QString & name, double coords[3] )
{
    // transform coords to local
    double localCoords[3];
    this->WorldToLocal( coords, localCoords );

    AddPointLocal( localCoords, name );
}

vtkActor * PointsObject::DoPicking( int x, int y, vtkRenderer * ren, double pickedPoint[3] )
{
    int validPickedPoint = m_picker->Pick( x, y, 0, ren );
    double pickPosition[4];
    m_picker->GetPickPosition( pickPosition );
    pickPosition[3] = 1;

    // Transform the point in the space of its parent PointsObject
    // Apply inverse of points object world transform to worldPoint
    // World transform may have multiple concatenations
    // For some reason when we try to GetLinearInverse() of the WorldTransform with 2 or more concatenations,
    // we get wron invewrse e.g world is concatenation of: identity, t1, t2 - we get inverse of t1
    // while if we go for the inverse matrix, it is correct
    vtkSmartPointer<vtkMatrix4x4> inverseMat = vtkSmartPointer<vtkMatrix4x4>::New();
    this->GetWorldTransform()->GetInverse( inverseMat );

    double transformedPoint[4];
    inverseMat->MultiplyPoint( pickPosition, transformedPoint );

    pickedPoint[0] = transformedPoint[0];
    pickedPoint[1] = transformedPoint[1];
    pickedPoint[2] = transformedPoint[2];

    if( validPickedPoint ) return m_picker->GetActor();
    return 0;
}

bool PointsObject::OnLeftButtonPressed( View * v, int x, int y, unsigned modifiers )
{
    // Make sure object is pickable first and shift button is pressed
    bool shift = ( modifiers & ShiftModifier ) != 0;
    if( !m_pickable || !shift || this->IsHidden() ) return false;

    double realPosition[3];
    vtkActor * picked = DoPicking( x, y, v->GetRenderer(), realPosition );
    if( picked )
    {
        // Find point clicked
        int pointIndex = FindPoint( picked, realPosition, v->GetType() );

        // No point is close to where we clicked -> add point
        if( pointIndex == PointsObject::InvalidPointIndex )
        {
            this->AddPointLocal( realPosition );
            SetSelectedPoint( GetNumberOfPoints() - 1 );
            MoveCursorToPoint( GetNumberOfPoints() - 1 );
        }
        else
        {
            SetSelectedPoint( pointIndex );
            m_movingPointIndex = pointIndex;
            MoveCursorToPoint( m_movingPointIndex );
        }
    }

    return true;
}

bool PointsObject::OnLeftButtonReleased( View * v, int x, int y, unsigned modifiers )
{
    if( m_movingPointIndex == PointsObject::InvalidPointIndex ) return false;

    double realPosition[3];
    vtkActor * picked = DoPicking( x, y, v->GetRenderer(), realPosition );
    if( picked ) SetPointCoordinates( m_movingPointIndex, realPosition );

    MoveCursorToPoint( m_movingPointIndex );
    UpdatePointsVisibility();
    m_movingPointIndex = PointsObject::InvalidPointIndex;

    return true;
}

bool PointsObject::OnRightButtonPressed( View * v, int x, int y, unsigned modifiers )
{
    bool shift = ( modifiers & ShiftModifier ) != 0;
    if( !m_pickable || !shift || this->IsHidden() ) return false;

    double realPosition[3];
    vtkActor * picked = DoPicking( x, y, v->GetRenderer(), realPosition );
    if( picked )
    {
        int pickedPointIndex = this->FindPoint( picked, realPosition, v->GetType() );
        if( pickedPointIndex > PointsObject::InvalidPointIndex ) this->RemovePoint( pickedPointIndex );
    }

    return true;
}

bool PointsObject::OnMouseMoved( View * v, int x, int y, unsigned modifiers )
{
    if( m_movingPointIndex == PointsObject::InvalidPointIndex ) return false;

    double realPosition[3];
    vtkActor * picked = DoPicking( x, y, v->GetRenderer(), realPosition );
    if( picked ) SetPointCoordinates( m_movingPointIndex, realPosition );

    return true;
}

void PointsObject::AddPointLocal( double coords[3], QString name, QString timestamp )
{
    if( name.isEmpty() ) name = QString::number( GetNumberOfPoints() + 1 );

    if( timestamp.isEmpty() ) timestamp = QDateTime::currentDateTime().toString( Qt::ISODate );

    // Set properties
    m_pointNames.append( name );
    m_pointCoordinates->InsertNextPoint( coords );
    m_timeStamps.append( timestamp );

    // Create point representation
    vtkSmartPointer<PointRepresentation> pr = vtkSmartPointer<PointRepresentation>::New();
    pr->SetListable( false );
    pr->SetName( name );
    int index = m_pointList.count();
    pr->SetPointIndex( index );
    pr->SetPosition( coords );
    pr->SetLabel( name.toUtf8().data() );
    pr->SetActive( true );
    pr->ShowLabel( m_showLabels );
    pr->SetHidden( this->IsHidden() );

    m_pointList.append( pr );

    // Add PointRepresentation to scene if PointsObject is already in Scene
    if( GetManager() ) this->GetManager()->AddObject( pr, this );

    emit PointAdded();

    // simtodo : this needs to be done after PointAdded signal to make sure GUI doesn't
    // get updated until LandmarkRegistration object is notified.
    UpdatePointProperties( index );
}

vtkPoints * PointsObject::GetPoints() { return m_pointCoordinates; }

void PointsObject::SetSelectedPoint( int index )
{
    // Set selected point
    if( index != InvalidPointIndex && index < m_pointCoordinates->GetNumberOfPoints() )
        m_selectedPointIndex = index;
    else
        return;

    // Update points colors
    UpdatePoints();

    emit PointsChanged();
    emit ObjectModified();
}

void PointsObject::UnselectAllPoints()
{
    m_selectedPointIndex = InvalidPointIndex;
    // Update points colors
    UpdatePoints();
    emit PointsChanged();
    emit ObjectModified();
}

void PointsObject::MoveCursorToPoint( int index )
{
    Q_ASSERT( index != InvalidPointIndex && index < m_pointCoordinates->GetNumberOfPoints() );

    // Move cursor to selected point
    if( !this->IsHidden() )
    {
        double * pos = m_pointCoordinates->GetPoint( index );  // in PointsObject space
        double worldPos[3];
        this->GetWorldTransform()->TransformPoint( pos, worldPos );
        this->GetManager()->SetCursorWorldPosition( worldPos );
    }
}

int PointsObject::FindPoint( vtkActor * actor, double * pos, int viewType )
{
    Q_ASSERT( this->GetManager() );

    if( m_pointList.isEmpty() ) return InvalidPointIndex;

    int n = m_pointList.count();
    double pointPosition[3];
    vtkSmartPointer<PointRepresentation> point;
    // first check if 3D actor was picked
    int i = 0;
    while( i < n )
    {
        point = m_pointList.value( i );
        if( point->HasActor( actor ) )
        {
            return i;
        }
        i++;
    }
    if( viewType == THREED_VIEW_TYPE ) return InvalidPointIndex;
    // Now see if any of 2D actors are picked
    vtkTransform * wt = this->GetWorldTransform();
    double worldPicked[3], worldPt[3];
    wt->TransformPoint( pos, worldPicked );
    for( int i = 0; i < n; i++ )
    {
        point = m_pointList.value( i );
        point->GetPosition( pointPosition );
        wt->TransformPoint( pointPosition, worldPt );
        bool isInPlane  = this->GetManager()->IsInPlane( (VIEWTYPES)viewType, worldPt );
        bool isInRadius = sqrt( vtkMath::Distance2BetweenPoints( worldPicked, worldPt ) ) < m_pointRadius2D;
        if( isInPlane && isInRadius ) return i;
    }
    return InvalidPointIndex;
}

void PointsObject::UpdatePointsVisibility()
{
    for( int i = 0; i < m_pointList.size(); ++i )
    {
        m_pointList[i]->UpdateVisibility();
    }
}

void PointsObject::OnCurrentObjectChanged()
{
    if( !m_pickabilityLocked )
    {
        if( GetManager()->GetCurrentObject() == this )
        {
            m_pickable = true;
            this->UpdatePointsVisibility();
        }
        else
            m_pickable = false;
    }
}

// properties
void PointsObject::Set3DRadius( double r )
{
    if( m_pointRadius3D == r ) return;
    if( r < MinRadius ) r = MinRadius;
    if( r > MaxRadius ) r = MaxRadius;
    m_pointRadius3D = r;
    this->UpdatePoints();
}

void PointsObject::Set2DRadius( double r )
{
    if( m_pointRadius2D == r ) return;
    m_pointRadius2D = r;
    this->UpdatePoints();
}

void PointsObject::SetLabelSize( double s )
{
    if( m_labelSize == s ) return;
    if( s < MinLabelSize ) s = MinLabelSize;
    if( s > MaxLabelSize ) s = MaxLabelSize;
    m_labelSize = s;
    this->UpdatePoints();
}

void PointsObject::SetEnabledColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) m_activeColor[i] = color[i];
    this->UpdatePoints();
}

void PointsObject::GetEnabledColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) color[i] = m_activeColor[i];
}

void PointsObject::SetDisabledColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) m_inactiveColor[i] = color[i];
    this->UpdatePoints();
}

void PointsObject::GetDisabledColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) color[i] = m_inactiveColor[i];
}

void PointsObject::SetSelectedColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) m_selectedColor[i] = color[i];
    this->UpdatePoints();
}

void PointsObject::GetSelectedColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) color[i] = m_selectedColor[i];
}

void PointsObject::SetOpacity( double opacity )
{
    m_opacity = opacity;
    this->UpdatePoints();
}

void PointsObject::ShowLabels( bool on )
{
    m_showLabels = on;
    this->UpdatePoints();
}

// access to individual points
void PointsObject::RemovePoint( int index )
{
    Q_ASSERT( index >= 0 && index < m_pointCoordinates->GetNumberOfPoints() );

    // Clear local data about the point
    vtkPoints * tmpPoints = vtkPoints::New();
    tmpPoints->DeepCopy( m_pointCoordinates );
    m_pointCoordinates->Reset();
    for( int i = 0; i < tmpPoints->GetNumberOfPoints(); ++i )
        if( i != index ) m_pointCoordinates->InsertNextPoint( tmpPoints->GetPoint( i ) );
    tmpPoints->Delete();
    m_pointNames.erase( m_pointNames.begin() + index );
    m_timeStamps.erase( m_timeStamps.begin() + index );

    // Update point representations
    if( GetManager() ) GetManager()->RemoveObject( m_pointList[index] );
    m_pointList.removeAt( index );
    for( int i = 0; i < m_pointList.size(); ++i ) m_pointList[i]->SetPointIndex( i );

    // Update selected point index
    if( m_selectedPointIndex >= GetNumberOfPoints() )
    {
        if( GetNumberOfPoints() > 0 )
        {
            m_selectedPointIndex = GetNumberOfPoints() - 1;
        }
        else
        {
            m_selectedPointIndex = InvalidPointIndex;
            emit ObjectModified();  // last point removed, we have to notify renderers
        }
    }

    this->UpdatePoints();
    emit PointRemoved( index );
}

void PointsObject::EnableDisablePoint( int index, bool enable )
{
    Q_ASSERT( index >= 0 && index < m_pointList.count() );
    vtkSmartPointer<PointRepresentation> pt = m_pointList.at( index );
    pt->SetActive( enable );
    this->UpdatePointProperties( index );
}

void PointsObject::UpdatePointProperties( int index )
{
    Q_ASSERT( index >= 0 && index < m_pointList.count() );
    vtkSmartPointer<PointRepresentation> pt = m_pointList.at( index );
    pt->SetLabel( m_pointNames.at( index ).toUtf8().data() );
    pt->SetPosition( m_pointCoordinates->GetPoint( index ) );
    pt->SetPointSizeIn3D( m_pointRadius3D );
    pt->SetPointSizeIn2D( m_pointRadius2D );
    pt->SetLabelScale( m_labelSize );
    pt->SetOpacity( m_opacity );
    pt->ShowLabel( m_showLabels );
    if( index == m_selectedPointIndex && pt->GetActive() )
        pt->SetPropertyColor( m_selectedColor );
    else if( pt->GetActive() && !m_pickabilityLocked )
        pt->SetPropertyColor( m_activeColor );
    else
        pt->SetPropertyColor( m_inactiveColor );
    emit ObjectModified();
}

void PointsObject::UpdatePoints()
{
    for( int i = 0; i < m_pointList.count(); i++ )
    {
        this->UpdatePointProperties( i );
    }
}

void PointsObject::SetPointLabel( int index, const QString & label )
{
    m_pointNames.replace( index, label );
    this->UpdatePointProperties( index );
    emit ObjectModified();
}

const QString PointsObject::GetPointLabel( int index ) { return m_pointNames.at( index ); }

double * PointsObject::GetPointCoordinates( int index ) { return m_pointCoordinates->GetPoint( index ); }

void PointsObject::GetPointCoordinates( int index, double coords[3] ) { m_pointCoordinates->GetPoint( index, coords ); }

void PointsObject::SetPointCoordinates( int index, double coords[3] )
{
    Q_ASSERT( index >= 0 && index < m_pointCoordinates->GetNumberOfPoints() );
    m_pointCoordinates->SetPoint( index, coords );
    m_pointList.at( index )->SetPosition( m_pointCoordinates->GetPoint( index ) );
    emit PointsChanged();
    emit ObjectModified();
}

void PointsObject::SetPointTimeStamp( int index, const QString & stamp )
{
    if( index >= 0 && index < m_pointCoordinates->GetNumberOfPoints() ) m_timeStamps.replace( index, stamp );
}

void PointsObject::ObjectAddedToScene()
{
    Q_ASSERT( GetManager() );

    // add all point representations to scene
    for( int i = 0; i < m_pointList.size(); ++i )
    {
        GetManager()->AddObject( m_pointList[i], this );
    }

    connect( this->GetManager(), SIGNAL( CurrentObjectChanged() ), this, SLOT( OnCurrentObjectChanged() ) );
    connect( this, SIGNAL( WorldTransformChangedSignal() ), this, SLOT( UpdatePointsVisibility() ) );
    connect( this->GetManager(), SIGNAL( CursorPositionChanged() ), this, SLOT( UpdatePointsVisibility() ) );
    connect( this->GetManager(), SIGNAL( ReferenceTransformChanged() ), this, SLOT( UpdatePointsVisibility() ) );
}

void PointsObject::ObjectAboutToBeRemovedFromScene()
{
    disconnect( this->GetManager(), SIGNAL( CurrentObjectChanged() ), this, SLOT( OnCurrentObjectChanged() ) );
    disconnect( this, SIGNAL( WorldTransformChangedSignal() ), this, SLOT( UpdatePointsVisibility() ) );
    disconnect( this->GetManager(), SIGNAL( CursorPositionChanged() ), this, SLOT( UpdatePointsVisibility() ) );
    disconnect( this->GetManager(), SIGNAL( ReferenceTransformChanged() ), this, SLOT( UpdatePointsVisibility() ) );
}

// Distance will be properly computed for points picked on unregistered objects
void PointsObject::ComputeDistanceFromSelectedPointToPointerTip()
{
    SceneManager * manager = this->GetManager();
    Q_ASSERT( manager );
    PointerObject * pointer = manager->GetNavigationPointerObject();
    double pointerPos[3], *pos1 = pointer->GetTipPosition();
    double pointPos[3], *pos2   = this->GetPointCoordinates( m_selectedPointIndex );
    for( int i = 0; i < 3; i++ )
    {
        pointerPos[i] = pos1[i];
        pointPos[i]   = pos2[i];
    }
    double tmpPos[3];
    vtkTransform * transform = this->GetLocalTransform();
    transform->TransformPoint( pointPos, tmpPos );
    // convert to world
    double worldCoords[3];
    this->LocalToWorld( tmpPos, worldCoords );
    double distance = sqrt( vtkMath::Distance2BetweenPoints( worldCoords, pointerPos ) );
    // Setup the text and add it to the window
    QString text = QString( "%1" ).arg( distance, -5, 'f', 0 );
    manager->SetGenericLabelText( text );
    this->LineToPointerTip( worldCoords, pointerPos );
}

void PointsObject::UpdateDistance()
{
    // Set button color according to navigation pointer state and availability of points to capture
    SceneManager * manager = this->GetManager();
    Q_ASSERT( manager );
    PointerObject * pObj = manager->GetNavigationPointerObject();
    if( pObj && pObj->IsOk() )
    {
        int numPoints = this->GetNumberOfPoints();
        if( numPoints > 0 )
        {
            if( m_computeDistance )
            {
                this->ComputeDistanceFromSelectedPointToPointerTip();
                manager->EmitShowGenericLabelText();
            }
        }
    }
}

void PointsObject::EnableComputeDistance( bool enable )
{
    m_computeDistance = enable;
    if( m_computeDistance )
    {
        m_lineToPointerTip = vtkSmartPointer<PolyDataObject>::New();
        m_lineToPointerTip->SetListable( false );
        m_lineToPointerTip->SetCanEditTransformManually( false );
        m_lineToPointerTip->SetObjectManagedByTracker( true );
        m_lineToPointerTip->SetColor( m_lineToPointerColor[0], m_lineToPointerColor[1], m_lineToPointerColor[2] );
        double pointPos[3];
        this->GetPointCoordinates( m_selectedPointIndex, pointPos );
        double worldCoords[3];
        this->LocalToWorld( pointPos, worldCoords );
        this->LineToPointerTip( worldCoords, worldCoords );
        this->GetManager()->AddObject( m_lineToPointerTip );
        connect( &( Application::GetInstance() ), SIGNAL( IbisClockTick() ), this, SLOT( UpdateDistance() ) );
    }
    else
    {
        disconnect( &( Application::GetInstance() ), SIGNAL( IbisClockTick() ), this, SLOT( UpdateDistance() ) );
        this->GetManager()->RemoveObject( m_lineToPointerTip );
        m_lineToPointerTip = 0;
    }
}

void PointsObject::LineToPointerTip( double selectedPoint[3], double pointerTip[3] )
{
    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints( 2 );
    points->SetPoint( 0, selectedPoint[0], selectedPoint[1], selectedPoint[2] );
    points->SetPoint( 1, pointerTip[0], pointerTip[1], pointerTip[2] );

    vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType pts[2];
    pts[0] = 0;
    pts[1] = 1;
    cells->InsertNextCell( 2, pts );

    vtkSmartPointer<vtkPolyData> linesPolyData = vtkSmartPointer<vtkPolyData>::New();
    linesPolyData->SetPoints( points );
    linesPolyData->SetLines( cells );

    m_lineToPointerTip->SetPolyData( linesPolyData );
    emit ObjectModified();
}

void PointsObject::SetLineToPointerColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) m_lineToPointerColor[i] = color[i];
    emit ObjectModified();
}

void PointsObject::GetLineToPointerColor( double color[3] )
{
    for( int i = 0; i < 3; i++ ) color[i] = m_lineToPointerColor[i];
}
