/*=========================================================================
Ibis Neuronav
Copyright (c) Simon Drouin, Anna Kochanowska, Louis Collins.
All rights reserved.
See Copyright.txt or http://ibisneuronav.org/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/
// Thanks to Dante De Nigris for writing this class

#ifndef GPU_VOLUMERECONSTRUCTIONWIDGET_H
#define GPU_VOLUMERECONSTRUCTIONWIDGET_H
#include <vtkSmartPointer.h>

#include <QWidget>
#include <QtGui>

#include "gpu_volumereconstruction.h"
#include "ibisitkvtkconverter.h"
#include "ui_gpu_volumereconstructionwidget.h"

class GPU_VolumeReconstructionPluginInterface;

namespace Ui
{
class GPU_VolumeReconstructionWidget;
}

class GPU_VolumeReconstructionWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GPU_VolumeReconstructionWidget( QWidget * parent = 0 );
    ~GPU_VolumeReconstructionWidget();

    void SetPluginInterface( GPU_VolumeReconstructionPluginInterface * ifc );

private:
    void UpdateUi();

    Ui::GPU_VolumeReconstructionWidget * ui;
    GPU_VolumeReconstruction::VolumeReconstructionPointer m_Reconstructor;
    QElapsedTimer m_ReconstructionTimer;
    GPU_VolumeReconstruction * m_VolumeReconstructor;
    GPU_VolumeReconstructionPluginInterface * m_pluginInterface;

private slots:

    void on_startButton_clicked();
    void slot_finished();
};

#endif
