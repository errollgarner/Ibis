/*=========================================================================
Ibis Neuronav
Copyright (c) Simon Drouin, Anna Kochanowska, Louis Collins.
All rights reserved.
See Copyright.txt or http://ibisneuronav.org/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/
#include "usacquisitionobject.h"

#include <itkMetaDataDictionary.h>
#include <itkMetaDataObject.h>
#include <vtkActor.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkImageActor.h>
#include <vtkImageConstantPad.h>  // added Mar 2, 2016, Xiao
#include <vtkImageData.h>
#include <vtkImageLuminance.h>
#include <vtkImageMapToColors.h>
#include <vtkImageMapper3D.h>
#include <vtkImageProperty.h>
#include <vtkImageShiftScale.h>
#include <vtkImageStencil.h>
#include <vtkImageToImageStencil.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPassThrough.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkTransform.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QMessageBox>
#include <QProgressDialog>
#include <iostream>
#include <string>

#include "application.h"
#include "exportacquisitiondialog.h"
#include "ibisconfig.h"
#include "imageobject.h"
#include "lookuptablemanager.h"
#include "serializerhelper.h"
#include "trackedvideobuffer.h"
#include "usacquisitionsettingswidget.h"
#include "usmask.h"
#include "usmasksettingswidget.h"
#include "view.h"
#include "vtkPiecewiseFunctionLookupTable.h"
#include "vtkXFMReader.h"
#include "vtkXFMWriter.h"

ObjectSerializationMacro( USAcquisitionObject );

USAcquisitionObject::USAcquisitionObject()
{
    m_usProbeObjectId = SceneManager::InvalidId;

    m_videoBuffer = new TrackedVideoBuffer( m_defaultImageSize[0], m_defaultImageSize[1] );

    m_isRecording   = false;
    m_baseDirectory = QDir::homePath() + "/" + IBIS_CONFIGURATION_SUBDIRECTORY + "/" + ACQ_BASE_DIR;

    m_usDepth         = "9cm";
    m_acquisitionType = UsProbeObject::ACQ_B_MODE;

    // current slice
    m_calibrationTransform = vtkSmartPointer<vtkTransform>::New();
    m_sliceTransform       = vtkSmartPointer<vtkTransform>::New();
    m_sliceTransform->Concatenate( this->GetWorldTransform() );
    m_currentImageTransform = vtkSmartPointer<vtkTransform>::New();
    m_sliceTransform->Concatenate( m_currentImageTransform );
    m_sliceTransform->Concatenate( m_calibrationTransform );
    m_sliceProperties = vtkSmartPointer<vtkImageProperty>::New();
    m_sliceLutIndex   = 1;  // default to hot metal
    m_lut             = vtkSmartPointer<vtkPiecewiseFunctionLookupTable>::New();
    m_lut->SetIntensityFactor( 1.0 );
    m_mapToColors = vtkSmartPointer<vtkImageMapToColors>::New();
    m_mapToColors->SetLookupTable( m_lut );
    m_mapToColors->SetOutputFormatToRGBA();
    m_mapToColors->SetInputConnection( m_videoBuffer->GetVideoOutputPort() );

    m_mask = USMask::New();  // default mask

    m_imageStencilSource = vtkSmartPointer<vtkImageToImageStencil>::New();
    m_imageStencilSource->SetInputData( m_mask->GetMask() );
    m_imageStencilSource->ThresholdByUpper( 128.0 );
    m_imageStencilSource->UpdateWholeExtent();

    m_sliceStencil = vtkSmartPointer<vtkImageStencil>::New();
    m_sliceStencil->SetStencilData( m_imageStencilSource->GetOutput() );
    m_sliceStencil->SetInputConnection( m_mapToColors->GetOutputPort() );
    m_sliceStencil->SetBackgroundColor( 1.0, 1.0, 1.0, 0.0 );

    m_isMaskOn = true;

    m_constantPad = vtkSmartPointer<vtkImageConstantPad>::New();
    m_constantPad->SetConstant( 255 );
    m_constantPad->SetOutputNumberOfScalarComponents( 4 );
    m_constantPad->SetInputConnection( m_videoBuffer->GetVideoOutputPort() );
    m_isDopplerOn = false;  // default state is B-mode

    m_sliceStencilDoppler = vtkSmartPointer<vtkImageStencil>::New();
    m_sliceStencilDoppler->SetStencilData( m_imageStencilSource->GetOutput() );
    m_sliceStencilDoppler->SetInputConnection( m_constantPad->GetOutputPort() );
    m_sliceStencilDoppler->SetBackgroundColor( 1.0, 1.0, 1.0, 0.0 );

    // Outputs
    m_maskedImageOutput = vtkSmartPointer<vtkPassThrough>::New();
    m_maskedImageOutput->SetInputConnection( m_sliceStencil->GetOutputPort() );
    m_unmaskedImageOutput = vtkSmartPointer<vtkPassThrough>::New();
    m_unmaskedImageOutput->SetInputConnection( m_mapToColors->GetOutputPort() );

    // static slices
    m_staticSlicesEnabled        = false;
    m_numberOfStaticSlices       = 2;  // default = first and last
    m_staticSlicesProperties     = vtkSmartPointer<vtkImageProperty>::New();
    m_staticSlicesLutIndex       = 0;  // default to greyscale
    m_staticSlicesDataNeedUpdate = true;
    m_defaultImageSize[0]        = 640;
    m_defaultImageSize[1]        = 480;
    m_componentsNumber           = 0;
    m_useCalibratedTransform     = false;
}

USAcquisitionObject::~USAcquisitionObject()
{
    disconnect( this );

    m_mask->Delete();
    ClearStaticSlicesData();

    delete m_videoBuffer;
}

void USAcquisitionObject::ObjectAddedToScene()
{
    SetSliceLutIndex( m_sliceLutIndex );
    SetStaticSlicesLutIndex( m_staticSlicesLutIndex );
    connect( m_mask, SIGNAL( MaskChanged() ), this, SLOT( UpdateMask() ) );
}

void USAcquisitionObject::SetUsProbe( UsProbeObject * probe )
{
    m_acquisitionType = probe->GetAcquisitionType();
    m_usDepth         = probe->GetCurrentCalibrationMatrixName();
    SetCalibrationMatrix( probe->GetCurrentCalibrationMatrix() );
    *m_mask           = *( probe->GetMask() );
    m_usProbeObjectId = probe->GetObjectID();
}

void USAcquisitionObject::Setup( View * view )
{
    this->SceneObject::Setup( view );

    if( view->GetType() == THREED_VIEW_TYPE )
    {
        PerViewElements elem;
        elem.imageSlice = vtkImageActor::New();
        elem.imageSlice->SetUserTransform( m_sliceTransform );
        elem.imageSlice->SetVisibility( !this->IsHidden() && this->GetNumberOfSlices() > 0 ? 1 : 0 );
        elem.imageSlice->SetProperty( m_sliceProperties );
        if( m_isMaskOn )
            elem.imageSlice->GetMapper()->SetInputConnection( m_sliceStencil->GetOutputPort() );
        else
            elem.imageSlice->GetMapper()->SetInputConnection( m_mapToColors->GetOutputPort() );

        view->GetRenderer()->AddActor( elem.imageSlice );

        SetupAllStaticSlices( view, elem );

        m_perViews[view] = elem;
    }
}

void USAcquisitionObject::Release( View * view )
{
    SceneObject::Release( view );

    PerViewContainer::iterator it = m_perViews.begin();
    while( it != m_perViews.end() )
    {
        View * view               = ( *it ).first;
        vtkRenderer * ren         = view->GetRenderer();
        PerViewElements & perView = ( *it ).second;
        ren->RemoveActor( perView.imageSlice );
        perView.imageSlice->Delete();
        ReleaseAllStaticSlices( view, perView );
        ++it;
    }

    m_perViews.clear();
}

void USAcquisitionObject::Hide()
{
    PerViewContainer::iterator it = m_perViews.begin();
    while( it != m_perViews.end() )
    {
        PerViewElements & perView = ( *it ).second;
        perView.imageSlice->VisibilityOff();
        HideStaticSlices( perView );
        ++it;
    }

    emit ObjectModified();
}

void USAcquisitionObject::Show()
{
    if( this->GetNumberOfSlices() > 0 )
    {
        PerViewContainer::iterator it = m_perViews.begin();
        while( it != m_perViews.end() )
        {
            PerViewElements & perView = ( *it ).second;
            perView.imageSlice->VisibilityOn();
            if( m_staticSlicesEnabled ) ShowStaticSlices( perView );
            ++it;
        }
        emit ObjectModified();
    }
}

void USAcquisitionObject::SetUseMask( bool useMask )
{
    m_isMaskOn = useMask;
    UpdatePipeline();
}

void USAcquisitionObject::SetUseDoppler( bool useDoppler )
{
    m_isDopplerOn = useDoppler;
    UpdatePipeline();
}

void USAcquisitionObject::UpdatePipeline()
{
    if( !m_isDopplerOn )
    {
        m_maskedImageOutput->SetInputConnection( m_sliceStencil->GetOutputPort() );
        m_unmaskedImageOutput->SetInputConnection( m_mapToColors->GetOutputPort() );
    }
    else
    {
        m_maskedImageOutput->SetInputConnection( m_sliceStencilDoppler->GetOutputPort() );
        m_unmaskedImageOutput->SetInputConnection( m_constantPad->GetOutputPort() );
    }

    PerViewContainer::iterator it = m_perViews.begin();
    while( it != m_perViews.end() )
    {
        PerViewElements & perView = ( *it ).second;
        if( m_isMaskOn && !m_isDopplerOn )
            perView.imageSlice->GetMapper()->SetInputConnection( m_sliceStencil->GetOutputPort() );
        else if( !m_isMaskOn && !m_isDopplerOn )
            perView.imageSlice->GetMapper()->SetInputConnection( m_mapToColors->GetOutputPort() );
        else if( m_isMaskOn && m_isDopplerOn )
            perView.imageSlice->GetMapper()->SetInputConnection( m_sliceStencilDoppler->GetOutputPort() );
        else  // !m_isMaskOn && m_isDopplerOn
            perView.imageSlice->GetMapper()->SetInputConnection( m_constantPad->GetOutputPort() );

        for( unsigned i = 0; i < perView.staticSlices.size(); ++i )  // don't touch the static for now
        {
            vtkImageActor * staticActor = perView.staticSlices[i];
            if( m_isMaskOn )
                staticActor->GetMapper()->SetInputConnection( m_staticSlicesData[i].imageStencil->GetOutputPort() );  //
            else
                staticActor->GetMapper()->SetInputConnection( m_staticSlicesData[i].mapToColors->GetOutputPort() );
        }
        ++it;
    }

    emit ObjectModified();
}

void USAcquisitionObject::HideStaticSlices( PerViewElements & perView )
{
    std::vector<vtkImageActor *>::iterator it = perView.staticSlices.begin();
    while( it != perView.staticSlices.end() )
    {
        ( *it )->VisibilityOff();
        ++it;
    }
    emit ObjectModified();
}

void USAcquisitionObject::ShowStaticSlices( PerViewElements & perView )
{
    std::vector<vtkImageActor *>::iterator it = perView.staticSlices.begin();
    while( it != perView.staticSlices.end() )
    {
        ( *it )->VisibilityOn();
        ++it;
    }
    emit ObjectModified();
}

#include "hardwaremodule.h"

void USAcquisitionObject::Record()
{
    Q_ASSERT( !m_isRecording );
    m_isRecording = true;

    // Add the frame that was last captured by the system
    UsProbeObject * probe = UsProbeObject::SafeDownCast( GetManager()->GetObjectByID( m_usProbeObjectId ) );
    Q_ASSERT( probe );
    if( probe->IsOk() )
    {
        int * dims = probe->GetVideoOutput()->GetDimensions();
        this->SetFrameAndMaskSize( dims[0], dims[1] );
        m_videoBuffer->AddFrame( probe->GetVideoOutput(), probe->GetUncalibratedWorldTransform()->GetMatrix(),
                                 probe->GetLastTimestamp() );
    }

    // Start watching the clock for updates
    connect( &Application::GetInstance(), SIGNAL( IbisClockTick() ), this, SLOT( Updated() ) );

    // Disable static slices
    this->SetEnableStaticSlices( false );

    emit ObjectModified();
}

bool USAcquisitionObject::AddFrame( vtkImageData * image, vtkMatrix4x4 * mat, double timestamp )
{
    if( m_isRecording ) return false;

    // check if frame dimensions match
    int * dims = image->GetDimensions();
    if( m_videoBuffer->GetNumberOfFrames() )
    {
        if( ( dims[0] != m_defaultImageSize[0] ) || ( dims[1] != m_defaultImageSize[1] ) ) return false;
    }
    else
    {
        this->SetFrameAndMaskSize( dims[0], dims[1] );
    }

    // Add the frame
    m_videoBuffer->AddFrame( image, mat, timestamp );

    emit ObjectModified();
    return true;
}

void USAcquisitionObject::Updated()
{
    if( m_isRecording )
    {
        UsProbeObject * probe = UsProbeObject::SafeDownCast( GetManager()->GetObjectByID( m_usProbeObjectId ) );
        Q_ASSERT( probe );
        if( probe->IsOk() )
        {
            m_videoBuffer->AddFrame( probe->GetVideoOutput(), probe->GetUncalibratedWorldTransform()->GetMatrix(),
                                     probe->GetLastTimestamp() );
            emit ObjectModified();
        }
    }
}

void USAcquisitionObject::UpdateMask()
{
    int currentFrameIndex = m_videoBuffer->GetCurrentFrame();
    if( currentFrameIndex < 0 )  // no frames yet
        return;
    this->SetCurrentFrame( 0 );
    m_imageStencilSource->Update();
    m_mapToColors->Update();
    m_constantPad->Update();
    this->SetNumberOfStaticSlices( m_numberOfStaticSlices );
    this->SetCurrentFrame( currentFrameIndex );
}

void USAcquisitionObject::Stop()
{
    if( m_isRecording )
    {
        m_isRecording = false;
        disconnect( &Application::GetInstance(), SIGNAL( IbisClockTick() ), this, SLOT( Updated() ) );
    }
}

void USAcquisitionObject::SetCurrentFrame( int frameIndex )
{
    m_videoBuffer->SetCurrentFrame( frameIndex );
    m_currentImageTransform->SetMatrix( m_videoBuffer->GetCurrentMatrix() );
    m_sliceTransform->Update();
    emit ObjectModified();
}

void USAcquisitionObject::Clear()
{
    m_videoBuffer->Clear();
    emit ObjectModified();
}

QString USAcquisitionObject::GetAcquisitionTypeAsString()
{
    QString ret;
    switch( m_acquisitionType )
    {
        case UsProbeObject::ACQ_B_MODE:
            ret = "B-Mode";
            break;
        case UsProbeObject::ACQ_DOPPLER:
            ret = "Doppler";
            break;
        case UsProbeObject::ACQ_POWER_DOPPLER:
            ret = "Power Doppler";
            break;
        default:
            ret = "Unknown";
            break;
    }
    return ret;
}

QString USAcquisitionObject::GetAcquisitionColor()
{
    QString ret;
    int nbComp = m_videoBuffer->GetFrameNumberOfComponents();
    if( nbComp == 1 )
        ret = ACQ_COLOR_GRAYSCALE;
    else if( nbComp == 3 )
        ret = ACQ_COLOR_RGB;
    else
        ret = "Unknown";
    return ret;
}

vtkAlgorithmOutput * USAcquisitionObject::GetMaskedOutputPort() { return m_maskedImageOutput->GetOutputPort(); }

vtkAlgorithmOutput * USAcquisitionObject::GetUnmaskedOutputPort() { return m_unmaskedImageOutput->GetOutputPort(); }

void USAcquisitionObject::SetCalibrationMatrix( vtkMatrix4x4 * mat )
{
    vtkMatrix4x4 * matCopy = vtkMatrix4x4::New();
    matCopy->DeepCopy( mat );
    m_calibrationTransform->SetMatrix( matCopy );
    matCopy->Delete();
    emit ObjectModified();
}

vtkTransform * USAcquisitionObject::GetCalibrationTransform() { return m_calibrationTransform; }

vtkImageData * USAcquisitionObject::GetVideoOutput() { return m_videoBuffer->GetVideoOutput(); }

vtkTransform * USAcquisitionObject::GetTransform() { return m_sliceTransform; }

void USAcquisitionObject::SetupAllStaticSlicesInAllViews()
{
    PerViewContainer::iterator it = m_perViews.begin();
    while( it != m_perViews.end() )
    {
        SetupAllStaticSlices( it->first, it->second );
        ++it;
    }
}

void USAcquisitionObject::SetupAllStaticSlices( View * view, PerViewElements & perView )
{
    if( m_staticSlicesDataNeedUpdate ) ComputeAllStaticSlicesData();

    for( unsigned i = 0; i < m_staticSlicesData.size(); ++i )
    {
        PerStaticSlice & pss = m_staticSlicesData[i];

        vtkImageActor * imageActor = vtkImageActor::New();
        if( m_isMaskOn )
            imageActor->GetMapper()->SetInputConnection( pss.imageStencil->GetOutputPort() );
        else
            imageActor->GetMapper()->SetInputConnection( pss.mapToColors->GetOutputPort() );
        imageActor->SetProperty( m_staticSlicesProperties );
        imageActor->SetUserTransform( pss.transform );
        if( !this->IsHidden() && m_staticSlicesEnabled )
            imageActor->VisibilityOn();
        else
            imageActor->VisibilityOff();
        view->GetRenderer()->AddActor( imageActor );

        perView.staticSlices.push_back( imageActor );
    }
}

void USAcquisitionObject::ReleaseAllStaticSlicesInAllViews()
{
    PerViewContainer::iterator it = m_perViews.begin();
    while( it != m_perViews.end() )
    {
        ReleaseAllStaticSlices( it->first, it->second );
        ++it;
    }
}

void USAcquisitionObject::ReleaseAllStaticSlices( View * view, PerViewElements & perView )
{
    std::vector<vtkImageActor *>::iterator it = perView.staticSlices.begin();
    while( it != perView.staticSlices.end() )
    {
        vtkImageActor * actor = ( *it );
        view->GetRenderer()->RemoveActor( actor );
        actor->Delete();
        ++it;
    }
    perView.staticSlices.clear();
}

void USAcquisitionObject::ComputeAllStaticSlicesData()
{
    // clear old data
    ClearStaticSlicesData();

    // compute slices data at regular interval
    int nbSlices = this->GetNumberOfSlices();
    if( nbSlices > 1 )
    {
        double interval = double( nbSlices ) / m_numberOfStaticSlices;
        for( int i = 0; i < m_numberOfStaticSlices - 1; ++i )
        {
            int index = (int)floor( interval * i );
            ComputeOneStaticSliceData( index );
        }

        // Last slice
        ComputeOneStaticSliceData( nbSlices - 1 );

        SetStaticSlicesLutIndex( this->m_staticSlicesLutIndex );
        m_staticSlicesDataNeedUpdate = false;
    }
}

void USAcquisitionObject::ComputeOneStaticSliceData( int sliceIndex )
{
    PerStaticSlice pss;

    // Get the slice image and matrices
    vtkImageData * slice                   = m_videoBuffer->GetImage( sliceIndex );
    vtkMatrix4x4 * sliceUncalibratedMatrix = m_videoBuffer->GetMatrix( sliceIndex );

    // Compute the (masked) image
    pss.mapToColors = vtkImageMapToColors::New();
    pss.mapToColors->SetOutputFormatToRGBA();
    pss.mapToColors->SetInputData( slice );

    pss.imageStencil = vtkImageStencil::New();
    pss.imageStencil->SetStencilData( m_imageStencilSource->GetOutput() );
    pss.imageStencil->SetInputConnection( pss.mapToColors->GetOutputPort() );
    pss.imageStencil->SetBackgroundColor( 1.0, 1.0, 1.0, 0.0 );

    // compute the transform of the slice
    vtkTransform * sliceUncalibratedTransform = vtkTransform::New();
    sliceUncalibratedTransform->SetMatrix( sliceUncalibratedMatrix );
    pss.transform = vtkTransform::New();
    pss.transform->Concatenate( this->GetWorldTransform() );
    pss.transform->Concatenate( sliceUncalibratedTransform );
    pss.transform->Concatenate( m_calibrationTransform );
    pss.transform->Update();
    m_staticSlicesData.push_back( pss );

    // cleanup
    sliceUncalibratedTransform->Delete();
}

void USAcquisitionObject::ClearStaticSlicesData()
{
    for( unsigned i = 0; i < m_staticSlicesData.size(); ++i )
    {
        PerStaticSlice & pss = m_staticSlicesData[i];
        pss.mapToColors->Delete();
        pss.imageStencil->Delete();
        pss.transform->Delete();
    }
    m_staticSlicesData.clear();
}

void USAcquisitionObject::Save() { this->ExportTrackedVideoBuffer(); }

bool USAcquisitionObject::LoadFramesFromMINCFile( QStringList & allMINCFiles )
{
    QFileInfo fi( allMINCFiles.at( 0 ) );
    bool ok = false;
    if( !( fi.isReadable() ) )
    {
        QString message( tr( "No read permission on file: " ) );
        message.append( allMINCFiles.at( 0 ) );
        QMessageBox::critical( 0, "Error", message, QMessageBox::Ok );
        return false;
    }
    m_componentsNumber = Application::GetInstance().GetNumberOfComponents( allMINCFiles.at( 0 ) );
    if( m_componentsNumber == 1 )
        ok = this->LoadGrayFrames( allMINCFiles );
    else if( m_componentsNumber > 1 )
        ok = LoadRGBFrames( allMINCFiles );
    return ok;
}

bool USAcquisitionObject::LoadGrayFrames( QStringList & allMINCFiles )
{
    bool processOK = true;

    // Get the first frame and from it
    // calibration matrix, flag  telling if that matrix was applied, we don't bother with frame ID as it is a
    // consecutive number
    IbisItkUnsignedChar3ImageType::Pointer itkImage = IbisItkUnsignedChar3ImageType::New();
    if( !Application::GetInstance().GetGrayFrame( allMINCFiles.at( 0 ), itkImage ) ) return false;
    itk::MetaDataDictionary dictionary;
    // From the first frame find global data - acquisition:calibratioMatrix and acquisition:calibratioMatrixApplied
    dictionary = itkImage->GetMetaDataDictionary();
    std::string calMat, calMatUsed;
    ;
    itk::ExposeMetaData<std::string>( dictionary, "acquisition:calibratioMatrix", calMat );
    // in reality, we do not need the calibration matrix from the frame, as it is loaded from USAcquisitionObject as
    // m_calibrationTransform
    m_useCalibratedTransform = false;
    if( itk::ExposeMetaData<std::string>( dictionary, "acquisition:calibratioMatrixApplied", calMatUsed ) )
    {
        if( calMat == "1" ) m_useCalibratedTransform = true;
    }
    // Now get all frames as vtkImageData and timestamps of frames.
    QProgressDialog * progress = new QProgressDialog( "Importing frames", "Cancel", 0, allMINCFiles.count() );
    progress->setAttribute( Qt::WA_DeleteOnClose, true );
    progress->show();
    IbisItkVtkConverter * ItktovtkConverter = IbisItkVtkConverter::New();
    for( int i = 0; i < allMINCFiles.count() && processOK; ++i )
    {
        IbisItkUnsignedChar3ImageType::Pointer itkImage = IbisItkUnsignedChar3ImageType::New();
        if( Application::GetInstance().GetGrayFrame( allMINCFiles.at( i ), itkImage ) )
        {
            dictionary = itkImage->GetMetaDataDictionary();
            std::string value;
            double ts = 0.0;
            if( itk::ExposeMetaData<std::string>( dictionary, "acquisition:timestamp", value ) )
                ts = std::stod( value );
            vtkTransform * tr                   = vtkTransform::New();
            vtkSmartPointer<vtkImageData> frame = vtkSmartPointer<vtkImageData>::New();
            frame                               = ItktovtkConverter->ConvertItkImageToVtkImage( itkImage, tr );

            // create full transform and reset image step and origin in order to avoid
            // double translation and scaling and display slices correctly in double view
            vtkSmartPointer<vtkMatrix4x4> outputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            this->AdjustFrame( frame, tr->GetMatrix(), outputMatrix );
            m_videoBuffer->AddFrame( frame, outputMatrix, ts );

            progress->setValue( i );
            qApp->processEvents();
            if( progress->wasCanceled() )
            {
                QMessageBox::information( 0, "Importing frames", "Process cancelled", QMessageBox::Ok );
                processOK = false;
            }
        }
        else
            processOK = false;
    }

    ItktovtkConverter->Delete();
    progress->close();
    return processOK;
}

bool USAcquisitionObject::LoadRGBFrames( QStringList & allMINCFiles )
{
    bool processOK = true;

    // Get the first frame to find
    // calibration matrix, flag  telling if that matrix was applied, we don't bother with frame ID as it is a
    // consecutive number
    IbisRGBImageType::Pointer itkImage = IbisRGBImageType::New();
    if( !Application::GetInstance().GetRGBFrame( allMINCFiles.at( 0 ), itkImage ) ) return false;
    itk::MetaDataDictionary dictionary;
    // From the first frame find global data - acquisition:calibratioMatrix and acquisition:calibratioMatrixApplied
    dictionary = itkImage->GetMetaDataDictionary();
    std::string calMat, calMatUsed;
    ;
    itk::ExposeMetaData<std::string>( dictionary, "acquisition:calibratioMatrix", calMat );
    // in reality, we do not need the calibration matrix from the frame, as it is loaded from USAcquisitionObject as
    // m_calibrationTransform
    m_useCalibratedTransform = false;

    if( itk::ExposeMetaData<std::string>( dictionary, "acquisition:calibratioMatrixApplied", calMatUsed ) )
    {
        if( calMat == "1" ) m_useCalibratedTransform = true;
    }
    // Now get all frames as vtkImageData and timestamps of frames.
    QProgressDialog * progress = new QProgressDialog( "Importing frames", "Cancel", 0, allMINCFiles.count() );
    progress->setAttribute( Qt::WA_DeleteOnClose, true );
    progress->show();

    IbisItkVtkConverter * ItktovtkConverter = IbisItkVtkConverter::New();
    for( int i = 0; i < allMINCFiles.count() && processOK; ++i )
    {
        IbisRGBImageType::Pointer itkImage = IbisRGBImageType::New();
        if( Application::GetInstance().GetRGBFrame( allMINCFiles.at( i ), itkImage ) )
        {
            dictionary = itkImage->GetMetaDataDictionary();
            std::string value;
            double ts = 0.0;
            if( itk::ExposeMetaData<std::string>( dictionary, "acquisition:timestamp", value ) )
                ts = std::stod( value );
            vtkSmartPointer<vtkTransform> tr    = vtkTransform::New();
            vtkSmartPointer<vtkImageData> frame = vtkSmartPointer<vtkImageData>::New();
            frame                               = ItktovtkConverter->ConvertItkImageToVtkImage( itkImage, tr );

            // create full transform and reset image step and origin in order to avoid
            // double translation and scaling and display slices correctly in double view
            vtkSmartPointer<vtkMatrix4x4> outputMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            this->AdjustFrame( frame, tr->GetMatrix(), outputMatrix );
            m_videoBuffer->AddFrame( frame, outputMatrix, ts );

            progress->setValue( i );
            qApp->processEvents();
            if( progress->wasCanceled() )
            {
                QMessageBox::information( 0, "Importing frames", "Process cancelled", QMessageBox::Ok );
                processOK = false;
            }
        }
        else
            processOK = false;
    }

    ItktovtkConverter->Delete();
    progress->close();
    return processOK;
}

void USAcquisitionObject::AdjustFrame( vtkImageData * frame, vtkMatrix4x4 * inputMatrix, vtkMatrix4x4 * outputMatrix )
{
    double start[3], step[3];
    frame->GetOrigin( start );
    frame->GetSpacing( step );
    vtkSmartPointer<vtkTransform> localTransform = vtkSmartPointer<vtkTransform>::New();
    localTransform->SetMatrix( inputMatrix );
    localTransform->Translate( start );
    localTransform->Scale( step );
    frame->SetOrigin( 0, 0, 0 );
    frame->SetSpacing( 1, 1, 1 );
    outputMatrix->DeepCopy( localTransform->GetMatrix() );
}

bool USAcquisitionObject::LoadFramesFromMINCFile( Serializer * ser )
{
    QString baseFileName;
    QString baseDirName;
    QString subDirName;
    if( ser->FileVersionIsLowerThan( QString::number( 5.0 ) ) )
        baseFileName = this->GetName();
    else
        baseFileName = QString::number( this->GetObjectID() );
    baseDirName = this->GetBaseDirectory();
    subDirName  = baseDirName + "/" + baseFileName;
    if( !QFile::exists( subDirName ) )
    {
        QString accessError = tr( "Directory not found - " ) + subDirName;
        QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
        return false;
    }
    QDir dir( subDirName );
    QStringList allMINCFiles = dir.entryList( QStringList( "*.mnc" ), QDir::Files, QDir::Name );
    if( allMINCFiles.isEmpty() )
    {
        QString accessError = tr( "No acquisition found in  " ) + subDirName;
        QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
        return false;
    }

    QStringList allMincPaths;
    for( int i = 0; i < allMINCFiles.size(); ++i )
    {
        QString filename( subDirName );
        filename.append( "/" );
        filename.append( allMINCFiles.at( i ) );
        allMincPaths.push_back( filename );
    }

    return LoadFramesFromMINCFile( allMincPaths );
}

void USAcquisitionObject::Serialize( Serializer * ser )
{
    SceneObject::Serialize( ser );

    double currentSliceOpacity = 1.0;
    double staticSlicesOpacity = 1.0;
    int currentSlice           = 0;
    int acquisitionType        = (int)m_acquisitionType;
    if( !ser->IsReader() )
    {
        currentSlice        = this->GetCurrentSlice();
        currentSliceOpacity = m_sliceProperties->GetOpacity();
        staticSlicesOpacity = m_staticSlicesProperties->GetOpacity();
        QString relPath( "./" );
        relPath.append( m_baseDirectory.section( '/', -1 ) );
        this->SetBaseDirectory( relPath );
        this->Save();
    }

    ::Serialize( ser, "BaseDirectory", m_baseDirectory );
    if( ser->IsReader() )
    {
        if( m_baseDirectory.at( 0 ) == '.' ) m_baseDirectory.replace( 0, 1, ser->GetSerializationDirectory() );
        if( !QDir( m_baseDirectory ).exists() )
        {
            QString accessError = tr( "Cannot find acquisition directory: " ) + m_baseDirectory;
            QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
            return;
        }

        // in old formats, acquisitions were not deletable and were managed by the system. It
        // is no longer the case, so we have to enforce what is read in scene object.
        this->SetObjectManagedBySystem( false );
        this->SetObjectDeletable( true );
    }
    ::Serialize( ser, "AcquisitionType", acquisitionType );
    ::Serialize( ser, "UsDepth", m_usDepth );
    ::Serialize( ser, "CalibrationMatrix", GetCalibrationTransform()->GetMatrix() );
    ::Serialize( ser, "CurrentSlice", currentSlice );
    ::Serialize( ser, "SliceLutIndex", m_sliceLutIndex );
    ::Serialize( ser, "SliceOpacity", currentSliceOpacity );
    ::Serialize( ser, "StaticSlicesEnabled", m_staticSlicesEnabled );
    ::Serialize( ser, "NumberOfStaticSlices", m_numberOfStaticSlices );
    ::Serialize( ser, "StaticSlicesOpacity", staticSlicesOpacity );
    ::Serialize( ser, "StaticSlicesLutIndex", m_staticSlicesLutIndex );
    ::Serialize( ser, "IsMaskOn", m_isMaskOn );
    ::Serialize( ser, "Mask", m_mask );

    if( ser->IsReader() )
    {
        m_acquisitionType = (UsProbeObject::ACQ_TYPE)acquisitionType;
        SetSliceLutIndex( m_sliceLutIndex );
        SetStaticSlicesLutIndex( m_staticSlicesLutIndex );
        m_sliceProperties->SetOpacity( currentSliceOpacity );
        m_staticSlicesProperties->SetOpacity( staticSlicesOpacity );

        if( this->LoadFramesFromMINCFile( ser ) ) SetCurrentFrame( currentSlice );
        this->UpdateMask();
    }
}

void USAcquisitionObject::CreateSettingsWidgets( QWidget * parent, QVector<QWidget *> * widgets )
{
    UsAcquisitionSettingsWidget * w = new UsAcquisitionSettingsWidget( parent );
    w->setObjectName( "Properties" );
    w->SetUSAcquisitionObject( this );
    widgets->append( w );
    USMaskSettingsWidget * w1 = new USMaskSettingsWidget( parent );
    w1->setObjectName( "Mask" );
    w1->SetMask( m_mask );
    w1->DisableSetASDefault();
    widgets->append( w1 );
}

int USAcquisitionObject::GetSliceWidth() { return m_videoBuffer->GetFrameWidth(); }

int USAcquisitionObject::GetSliceHeight() { return m_videoBuffer->GetFrameHeight(); }

int USAcquisitionObject::GetNumberOfSlices() { return m_videoBuffer->GetNumberOfFrames(); }

vtkImageData * USAcquisitionObject::GetMask() { return m_mask->GetMask(); }

void USAcquisitionObject::GetFrameData( int index, vtkImageData * slice, vtkMatrix4x4 * calibratedSliceMatrix )
{
    Q_ASSERT_X( ( index >= 0 && index < m_videoBuffer->GetNumberOfFrames() ), "USAcquisitionObject::GetFrameData()",
                "index out of range" );
    Q_ASSERT_X( calibratedSliceMatrix, "USAcquisitionObject::GetFrameData()",
                "sliceMatrix must be allocated before this callL" );
    Q_ASSERT_X( slice, "USAcquisitionObject::GetFrameData()", "slice must be allocated before this callL" );
    int currentFrame = m_videoBuffer->GetCurrentFrame();
    this->SetCurrentFrame( index );
    calibratedSliceMatrix->DeepCopy( m_sliceTransform->GetMatrix() );
    this->SetCurrentFrame( currentFrame );
    slice->DeepCopy( m_videoBuffer->GetImage( index ) );
}

void USAcquisitionObject::GetItkImage( IbisItkUnsignedChar3ImageType::Pointer itkOutputImage, int frameNo, bool masked,
                                       bool useCalibratedTransform, int relativeToObjectID )
{
    Q_ASSERT_X( itkOutputImage, "USAcquisitionObject::GetItkImage()",
                "itkOutputImage must be allocated before this call" );

    // prepare transform
    vtkSmartPointer<vtkMatrix4x4> frameMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    frameMatrix->Identity();
    vtkSmartPointer<vtkMatrix4x4> calibratedFrameMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    calibratedFrameMatrix->Identity();
    vtkMatrix4x4::Multiply4x4( m_videoBuffer->GetMatrix( frameNo ), m_calibrationTransform->GetMatrix(),
                               calibratedFrameMatrix );

    vtkMatrix4x4 * relativeToMatrix = 0;
    if( relativeToObjectID != SceneManager::InvalidId )
    {
        SceneObject * relativeTo = this->GetManager()->GetObjectByID( relativeToObjectID );
        Q_ASSERT( relativeTo );
        relativeToMatrix = relativeTo->GetWorldTransform()->GetLinearInverse()->GetMatrix();
    }

    if( relativeToMatrix )
    {
        if( useCalibratedTransform )
        {
            vtkMatrix4x4::Multiply4x4( relativeToMatrix, calibratedFrameMatrix, frameMatrix );
        }
        else
            vtkMatrix4x4::Multiply4x4( relativeToMatrix, m_videoBuffer->GetMatrix( frameNo ), frameMatrix );
    }
    else
    {
        if( useCalibratedTransform )
        {
            frameMatrix->DeepCopy( calibratedFrameMatrix );
        }
        else
            frameMatrix->DeepCopy( m_videoBuffer->GetMatrix( frameNo ) );
    }

    // prepare image
    vtkImageData * initialImage                        = m_videoBuffer->GetImage( frameNo );
    int numberOfScalarComponents                       = initialImage->GetNumberOfScalarComponents();
    vtkImageData * grayImage                           = initialImage;
    vtkSmartPointer<vtkImageLuminance> luminanceFilter = vtkSmartPointer<vtkImageLuminance>::New();
    if( numberOfScalarComponents > 1 )
    {
        luminanceFilter->SetInputData( initialImage );
        luminanceFilter->Update();
        grayImage = luminanceFilter->GetOutput();
    }
    vtkImageData * image;
    vtkSmartPointer<vtkImageShiftScale> shifter = vtkSmartPointer<vtkImageShiftScale>::New();
    if( initialImage->GetScalarType() != VTK_UNSIGNED_CHAR )
    {
        shifter->SetOutputScalarType( VTK_UNSIGNED_CHAR );
        shifter->SetClampOverflow( 1 );
        shifter->SetInputData( grayImage );
        shifter->SetShift( 0 );
        shifter->SetScale( 1.0 );
        shifter->Update();
        image = shifter->GetOutput();
    }
    else
        image = initialImage;

    vtkSmartPointer<vtkImageStencil> sliceStencil = vtkSmartPointer<vtkImageStencil>::New();
    vtkImageData * imageToConvert                 = image;
    if( masked )
    {
        sliceStencil->SetStencilData( m_imageStencilSource->GetOutput() );
        sliceStencil->SetInputData( image );
        sliceStencil->SetBackgroundColor( 1.0, 1.0, 1.0, 0.0 );
        sliceStencil->Update();
        imageToConvert = sliceStencil->GetOutput();
    }

    // convert to ITK image
    vtkSmartPointer<IbisItkVtkConverter> converter = vtkSmartPointer<IbisItkVtkConverter>::New();
    converter->ConvertVtkImageToItkImage( itkOutputImage, imageToConvert, frameMatrix );
}

void USAcquisitionObject::GetItkRGBImage( IbisRGBImageType::Pointer itkOutputImage, int frameNo, bool masked,
                                          bool useCalibratedTransform, int relativeToObjectID )
{
    Q_ASSERT_X( itkOutputImage, "USAcquisitionObject::GetItkImage()",
                "itkOutputImage must be created before this call" );

    // prepare transform
    vtkSmartPointer<vtkMatrix4x4> frameMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    frameMatrix->Identity();
    vtkSmartPointer<vtkMatrix4x4> calibratedFrameMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    calibratedFrameMatrix->Identity();
    vtkMatrix4x4::Multiply4x4( m_videoBuffer->GetMatrix( frameNo ), m_calibrationTransform->GetMatrix(),
                               calibratedFrameMatrix );
    vtkMatrix4x4 * relativeToMatrix = 0;
    if( relativeToObjectID != SceneManager::InvalidId )
    {
        SceneObject * relativeTo = this->GetManager()->GetObjectByID( relativeToObjectID );
        Q_ASSERT( relativeTo );
        relativeToMatrix = relativeTo->GetWorldTransform()->GetLinearInverse()->GetMatrix();
    }
    if( relativeToMatrix )
    {
        if( useCalibratedTransform )
        {
            vtkMatrix4x4::Multiply4x4( relativeToMatrix, calibratedFrameMatrix, frameMatrix );
        }
        else
            vtkMatrix4x4::Multiply4x4( relativeToMatrix, m_videoBuffer->GetMatrix( frameNo ), frameMatrix );
    }
    else
    {
        if( useCalibratedTransform )
        {
            frameMatrix->DeepCopy( calibratedFrameMatrix );
        }
        else
            frameMatrix->DeepCopy( m_videoBuffer->GetMatrix( frameNo ) );
    }

    // prepare image
    vtkImageData * image                          = m_videoBuffer->GetImage( frameNo );
    vtkSmartPointer<vtkImageStencil> sliceStencil = vtkSmartPointer<vtkImageStencil>::New();
    vtkImageData * imageToConvert                 = image;
    if( masked )
    {
        sliceStencil->SetStencilData( m_imageStencilSource->GetOutput() );
        sliceStencil->SetInputData( image );
        sliceStencil->SetBackgroundColor( 1.0, 1.0, 1.0, 0.0 );
        sliceStencil->Update();
        imageToConvert = sliceStencil->GetOutput();
    }

    // convert to ITK image
    vtkSmartPointer<IbisItkVtkConverter> converter = vtkSmartPointer<IbisItkVtkConverter>::New();
    converter->ConvertVtkImageToItkImage( itkOutputImage, imageToConvert, frameMatrix );
}

#include <itkImageFileWriter.h>

void USAcquisitionObject::Export()
{
    ExportParams params;
    ExportAcquisitionDialog * dialog = new ExportAcquisitionDialog( 0, Qt::WindowStaysOnTopHint );
    dialog->setAttribute( Qt::WA_DeleteOnClose, true );
    dialog->SetUSAcquisitionObject( this );
    dialog->SetExportParams( &params );
    if( dialog->exec() == QDialog::Accepted )
        this->ExportTrackedVideoBuffer( params.outputDir, params.masked, params.useCalibratedTransform,
                                        params.relativeToID );
}

void USAcquisitionObject::ExportTrackedVideoBuffer( QString destDir, bool masked, bool useCalibratedTransform,
                                                    int relativeToID )
{
    Q_ASSERT( GetManager() );

    // have to provide for vtkImage only
    int numberOfFrames = m_videoBuffer->GetNumberOfFrames();
    // we have to take copy of current settings and change base directory
    QString partFileName;
    QString baseFileName;
    QString baseDirName( destDir );
    if( baseDirName.isEmpty() )
    {
        baseDirName = this->GetManager()->GetSceneDirectory();
        baseDirName.append( '/' );
        baseDirName.append( m_baseDirectory.section( '/', -1 ) );
    }
    QString subDirName;
    bool dirMade;
    baseFileName = QString::number( this->GetObjectID() );
    subDirName   = baseDirName + "/" + baseFileName;
    partFileName = subDirName + "/" + baseFileName;
    if( !QFile::exists( baseDirName ) )
    {
        QDir baseDir;
        dirMade = baseDir.mkpath( baseDirName );
        if( !dirMade )
        {
            QString accessError = tr( "Can't create directory:\n" ) + baseDirName;
            QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
            return;
        }
    }
    if( QFile::exists( subDirName ) )
    {
        QDir tmp( subDirName );
        QStringList allFiles = tmp.entryList( QStringList( "*.*" ), QDir::Files, QDir::Name );
        if( !allFiles.isEmpty() )
        {
            for( int i = 0; i < allFiles.size(); i++ ) tmp.remove( allFiles[i] );
        }
        if( !tmp.rmdir( subDirName ) )
        {
            QString accessError =
                tr( "Please select different directory.\nAcquisition data already saved in: " ) + subDirName;
            QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
            return;
        }
    }
    QDir subDir;
    dirMade = subDir.mkdir( subDirName );
    if( !dirMade )
    {
        QString accessError = tr( "Can't create directory:\n" ) + subDirName;
        QMessageBox::warning( 0, tr( "Error: " ), accessError, QMessageBox::Ok );
        return;
    }
    bool processOK             = false;
    QProgressDialog * progress = new QProgressDialog( "Exporting frames", "Cancel", 0, numberOfFrames );
    progress->setAttribute( Qt::WA_DeleteOnClose, true );
    // Prepare for writing out calibration matrix
    vtkMatrix4x4 * calMatrix = this->GetCalibrationTransform()->GetMatrix();
    QString calMatString;
    for( int i = 0; i < 3; i++ )
    {
        for( int j = 0; j < 4; j++ )
        {
            calMatString.append( QString::number( calMatrix->GetElement( i, j ), 'f', 6 ) );
            calMatString.append( " " );
        }
    }
    calMatString.replace( calMatString.size() - 1, 1, ";" );

    if( numberOfFrames > 0 )
    {
        int backupCurrentFrame = this->GetCurrentSlice();
        progress->show();
        processOK = true;

        int sequenceNumber = 0;  // make sure to number output files sequentially
        int nbComp         = m_videoBuffer->GetFrameNumberOfComponents();
        if( nbComp == 1 )
        {
            itk::ImageFileWriter<IbisItkUnsignedChar3ImageType>::Pointer mincWriter =
                itk::ImageFileWriter<IbisItkUnsignedChar3ImageType>::New();
            for( int i = 0; i < numberOfFrames && processOK; i++ )
            {
                QString Number( QString::number( ++sequenceNumber ) );
                int numLength = Number.length();
                QString numberedFileName( partFileName );
                numberedFileName += '.';
                for( int j = 0; j < 5 - numLength; j++ )
                {
                    numberedFileName += "0";
                }

                numberedFileName += Number;
                numberedFileName += ".mnc";
                mincWriter->SetFileName( numberedFileName.toUtf8().data() );

                IbisItkUnsignedChar3ImageType::Pointer itkSliceImage = IbisItkUnsignedChar3ImageType::New();
                this->GetItkImage( itkSliceImage, i, masked, useCalibratedTransform, relativeToID );
                // Output acquisition properties: time stamp, calibration matrix, frame ID, flag telling idf the
                // calibration matrix was applied
                double timestamp                   = m_videoBuffer->GetTimestamp( i );
                itk::MetaDataDictionary & metaDict = itkSliceImage->GetMetaDataDictionary();
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:calibratioMatrix",
                                                       calMatString.toUtf8().data() );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:calibratioMatrixApplied",
                                                       useCalibratedTransform ? "1" : "0" );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:timestamp",
                                                       QString::number( timestamp, 'f', 6 ).toUtf8().data() );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:frameID",
                                                       QString::number( sequenceNumber - 1 ).toUtf8().data() );
                mincWriter->SetInput( itkSliceImage );
                try
                {
                    mincWriter->Update();
                }
                catch( itk::ExceptionObject & exp )
                {
                    std::cerr << "Exception caught!" << std::endl;
                    std::cerr << exp << std::endl;
                    processOK = false;
                    break;
                }
                progress->setValue( i );
                qApp->processEvents();
                if( progress->wasCanceled() )
                {
                    QMessageBox::information( 0, tr( "Exporting frames" ), tr( "Process cancelled" ), QMessageBox::Ok );
                    processOK = false;
                }
            }
        }
        else
        {
            itk::ImageFileWriter<IbisRGBImageType>::Pointer mincWriter = itk::ImageFileWriter<IbisRGBImageType>::New();
            for( int i = 0; i < numberOfFrames && processOK; i++ )
            {
                QString Number( QString::number( ++sequenceNumber ) );
                int numLength = Number.length();
                QString numberedFileName( partFileName );
                numberedFileName += '.';
                for( int j = 0; j < 5 - numLength; j++ )
                {
                    numberedFileName += "0";
                }

                numberedFileName += Number;
                numberedFileName += ".mnc";
                mincWriter->SetFileName( numberedFileName.toUtf8().data() );

                IbisRGBImageType::Pointer itkSliceImage = IbisRGBImageType::New();
                this->GetItkRGBImage( itkSliceImage, i, masked, useCalibratedTransform, relativeToID );
                // Output acquisition properties: time stamp, calibration matrix, frame ID, flag telling idf the
                // calibration matrix was applied
                double timestamp                   = m_videoBuffer->GetTimestamp( i );
                itk::MetaDataDictionary & metaDict = itkSliceImage->GetMetaDataDictionary();
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:calibratioMatrix",
                                                       calMatString.toUtf8().data() );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:calibratioMatrixApplied",
                                                       useCalibratedTransform ? "1" : "0" );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:timestamp",
                                                       QString::number( timestamp, 'g', 6 ).toUtf8().data() );
                itk::EncapsulateMetaData<std::string>( metaDict, "acquisition:frameID",
                                                       QString::number( sequenceNumber - 1 ).toUtf8().data() );
                mincWriter->SetInput( itkSliceImage );

                try
                {
                    mincWriter->Update();
                }
                catch( itk::ExceptionObject & exp )
                {
                    std::cerr << "Exception caught!" << std::endl;
                    std::cerr << exp << std::endl;
                    processOK = false;
                    break;
                }
                progress->setValue( i );
                qApp->processEvents();
                if( progress->wasCanceled() )
                {
                    QMessageBox::information( 0, tr( "Exporting frames" ), tr( "Process cancelled" ), QMessageBox::Ok );
                    processOK = false;
                }
            }
        }
        progress->close();
        this->SetCurrentFrame( backupCurrentFrame );
    }
    if( !useCalibratedTransform )  // export calibration transform
    {
        QString calibrationTransformFileName( subDirName );
        calibrationTransformFileName.append( "/calibrationTransform.xfm" );
        vtkXFMWriter * writer = vtkXFMWriter::New();
        writer->SetFileName( calibrationTransformFileName.toUtf8().data() );
        writer->SetMatrix( m_calibrationTransform->GetMatrix() );
        writer->Write();
        writer->Delete();
    }
    if( !processOK ) QMessageBox::warning( 0, "Error: ", "Exporting frames failed.", QMessageBox::Ok );
}

bool USAcquisitionObject::Import()
{
    QStringList filenames;
    QString extension( ".mnc" );
    QString initialPath( Application::GetInstance().GetSettings()->WorkingDirectory );
    bool success = Application::GetInstance().GetOpenFileSequence(
        filenames, extension, "Select first file of acquisition", initialPath, "Minc file (*.mnc)" );
    if( success )
    {
        if( this->LoadFramesFromMINCFile( filenames ) )
        {
            this->SetCurrentFrame( 0 );
            // look for calibration transform
            m_calibrationTransform->Identity();
            QFileInfo fi( filenames.at( 0 ) );
            QStringList acqSuffix = fi.fileName().split( "." );
            QString acqName       = tr( "Acquisition_" ) + acqSuffix[0];
            this->SetName( acqName );
            this->SetFullFileName( fi.absoluteFilePath() );
            QString calibrationTransformFileName( fi.absolutePath() );
            calibrationTransformFileName.append( "/calibrationTransform.xfm" );
            if( QFile::exists( calibrationTransformFileName ) )
            {
                vtkXFMReader * reader = vtkXFMReader::New();
                if( reader->CanReadFile( calibrationTransformFileName.toUtf8() ) )
                {
                    vtkMatrix4x4 * mat = vtkMatrix4x4::New();
                    reader->SetFileName( calibrationTransformFileName.toUtf8() );
                    reader->SetMatrix( mat );
                    reader->Update();
                    m_calibrationTransform->SetMatrix( mat );
                    m_calibrationTransform->Update();
                }
                reader->Delete();
            }
        }
    }
    return success;
}

void USAcquisitionObject::SetFrameAndMaskSize( int width, int height )
{
    m_defaultImageSize[0] = width;
    m_defaultImageSize[1] = height;
    m_mask->SetMaskSize( width, height );
}

int USAcquisitionObject::GetCurrentSlice() { return m_videoBuffer->GetCurrentFrame(); }

void USAcquisitionObject::SetSliceImageOpacity( double opacity )
{
    m_sliceProperties->SetOpacity( opacity );
    emit ObjectModified();
}

double USAcquisitionObject::GetSliceImageOpacity() { return m_sliceProperties->GetOpacity(); }

void USAcquisitionObject::SetSliceLutIndex( int index )
{
    m_sliceLutIndex       = index;
    double range[2]       = { 0.0, 255.0 };
    QString slicesLutName = Application::GetLookupTableManager()->GetTemplateLookupTableName( m_sliceLutIndex );
    Application::GetLookupTableManager()->CreateLookupTable( slicesLutName, range, m_lut );
    emit ObjectModified();
}

void USAcquisitionObject::SetEnableStaticSlices( bool enable )
{
    if( m_staticSlicesEnabled == enable ) return;

    m_staticSlicesEnabled = enable;
    if( m_staticSlicesEnabled )
    {
        if( !this->IsHidden() )
        {
            PerViewContainer::iterator it = m_perViews.begin();
            while( it != m_perViews.end() )
            {
                PerViewElements & perView = ( *it ).second;
                ShowStaticSlices( perView );
                ++it;
            }
        }
    }
    else
    {
        PerViewContainer::iterator it = m_perViews.begin();
        while( it != m_perViews.end() )
        {
            PerViewElements & perView = ( *it ).second;
            HideStaticSlices( perView );
            ++it;
        }
    }
    emit ObjectModified();
}

void USAcquisitionObject::SetNumberOfStaticSlices( int nb )
{
    Q_ASSERT( nb >= 2 );
    m_numberOfStaticSlices = nb;
    ReleaseAllStaticSlicesInAllViews();
    m_staticSlicesDataNeedUpdate = true;
    SetupAllStaticSlicesInAllViews();
    emit ObjectModified();
}

void USAcquisitionObject::SetStaticSlicesOpacity( double opacity )
{
    m_staticSlicesProperties->SetOpacity( opacity );
    emit ObjectModified();
}

double USAcquisitionObject::GetStaticSlicesOpacity() { return m_staticSlicesProperties->GetOpacity(); }

void USAcquisitionObject::SetStaticSlicesLutIndex( int index )
{
    m_staticSlicesLutIndex = index;
    double range[2]        = { 0.0, 255.0 };
    QString staticSlicesLutName =
        Application::GetLookupTableManager()->GetTemplateLookupTableName( m_staticSlicesLutIndex );
    vtkPiecewiseFunctionLookupTable * staticLut = vtkPiecewiseFunctionLookupTable::New();
    staticLut->SetIntensityFactor( 1.0 );
    Application::GetLookupTableManager()->CreateLookupTable( staticSlicesLutName, range, staticLut );
    for( unsigned i = 0; i < m_staticSlicesData.size(); ++i )
    {
        m_staticSlicesData[i].mapToColors->SetLookupTable( staticLut );
    }
    staticLut->Delete();
    emit ObjectModified();
}

double USAcquisitionObject::GetFrameTimestamp( int index ) { return m_videoBuffer->GetTimestamp( index ); }

double USAcquisitionObject::GetCurrentFrameTimestamp() { return m_videoBuffer->GetCurrentTimestamp(); }
