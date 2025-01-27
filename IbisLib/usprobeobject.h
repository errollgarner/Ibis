/*=========================================================================
Ibis Neuronav
Copyright (c) Simon Drouin, Anna Kochanowska, Louis Collins.
All rights reserved.
See Copyright.txt or http://ibisneuronav.org/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/
#ifndef USPROBEOBJECT_H
#define USPROBEOBJECT_H

#include <vtkSmartPointer.h>

#include <QObject>
#include <map>

#include "hardwaremodule.h"
#include "trackedsceneobject.h"

class vtkImageData;
class vtkImageActor;
class vtkImageProperty;
class vtkImageMapToColors;
class vtkImageToImageStencil;
class vtkImageStencil;
class vtkImageConstantPad;
class vtkPassThrough;
class vtkAlgorithmOutput;
class USMask;

class UsProbeObject : public TrackedSceneObject
{
    Q_OBJECT

public:
    static UsProbeObject * New() { return new UsProbeObject; }
    vtkTypeMacro( UsProbeObject, TrackedSceneObject );

    UsProbeObject();
    virtual ~UsProbeObject();

    virtual void SerializeTracked( Serializer * ser ) override;

    void AddClient();
    void RemoveClient();

    void SetUseMask( bool useMask );
    bool GetUseMask() { return m_maskOn; }
    USMask * GetMask() { return m_mask; }

    // Implementation of standard SceneObject method
    virtual void Setup( View * view ) override;
    virtual void Release( View * view ) override;
    virtual void CreateSettingsWidgets( QWidget * parent, QVector<QWidget *> * widgets ) override;

    void SetVideoInputConnection( vtkAlgorithmOutput * port );
    void SetVideoInputData( vtkImageData * image );
    void UpdateVideoInput();

    int GetVideoImageWidth();
    int GetVideoImageHeight();
    int GetVideoImageNumberOfComponents();
    vtkImageData * GetVideoOutput();
    vtkAlgorithmOutput * GetVideoOutputPort();

    int GetNumberOfCalibrationMatrices();
    void SetCurrentCalibrationMatrixIndex( int index );
    QString GetCalibrationMatrixName( int index );
    void SetCurrentCalibrationMatrixName( QString name );
    QString GetCurrentCalibrationMatrixName();
    void SetCurrentCalibrationMatrix( vtkMatrix4x4 * mat );
    vtkMatrix4x4 * GetCurrentCalibrationMatrix();
    void AddCalibrationMatrix(
        QString name );  // the matrix will be identity, user may change it using calibrationMatrix button in settings

    enum ACQ_TYPE
    {
        ACQ_B_MODE        = 0,
        ACQ_DOPPLER       = 1,
        ACQ_POWER_DOPPLER = 2
    };
    void SetAcquisitionType( ACQ_TYPE type );
    ACQ_TYPE GetAcquisitionType() { return m_acquisitionType; }

    int GetNumberOfAvailableLUT();
    QString GetLUTName( int index );
    void SetCurrentLUTIndex( int index );
    int GetCurrentLUTIndex() { return m_lutIndex; }

    void TakeSnapshot();

    struct CalibrationMatrixInfo
    {
        CalibrationMatrixInfo();
        CalibrationMatrixInfo( const CalibrationMatrixInfo & other );
        ~CalibrationMatrixInfo();
        bool Serialize( Serializer * ser );
        QString name;
        vtkMatrix4x4 * matrix;
    };

private slots:

    void OnUpdate();
    void UpdateMask();

protected:
    void ObjectAddedToScene() override;
    void ObjectRemovedFromScene() override;
    virtual void Hide() override;
    virtual void Show() override;

    bool m_maskOn;
    int m_lutIndex;
    unsigned int m_screenShotIndex;

    vtkSmartPointer<vtkPassThrough> m_videoInput;
    vtkSmartPointer<vtkPassThrough> m_actorInput;
    USMask * m_mask;
    USMask * m_defaultMask;
    vtkSmartPointer<vtkImageMapToColors> m_mapToColors;
    vtkSmartPointer<vtkImageToImageStencil> m_imageStencilSource;
    vtkSmartPointer<vtkImageStencil> m_sliceStencil;
    vtkSmartPointer<vtkImageConstantPad> m_constantPad;

    vtkSmartPointer<vtkTransform> m_imageTransform;

    // Calibration matrices for different scale levels
    int m_currentCalibrationMatrixIndex;
    QList<CalibrationMatrixInfo> m_calibrationMatrices;

    struct PerViewElements
    {
        PerViewElements() : imageActor( 0 ) {}
        vtkImageActor * imageActor;
    };
    typedef std::map<View *, PerViewElements> PerViewContainer;
    PerViewContainer m_perViews;

    void UpdatePipeline();

    ACQ_TYPE m_acquisitionType;
};

ObjectSerializationHeaderMacro( UsProbeObject );
ObjectSerializationHeaderMacro( UsProbeObject::CalibrationMatrixInfo );

#endif
