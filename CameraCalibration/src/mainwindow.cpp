#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "camerathread.h"
#include "qopencvscene.h"

#include <QCameraInfo>
#include <QGLWidget>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <vector>

#include "qchessboardelab.h"
#include "qcameraundistort.h"

#include <iostream>

using namespace std;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    mCameraThread(NULL),
    mCameraSceneRaw(NULL),
    mCameraSceneCheckboard(NULL),
    mCameraSceneUndistorted(NULL),
    mCameraUndist(NULL)
{
    ui->setupUi(this);

    killGstLaunch();

    mCameraConnected = false;

    updateOpenCvVer();

    // >>>>> OpenCV version
    QString ocvVers = updateOpenCvVer();
    mOpenCvVer.setText( ocvVers );
    ui->statusBar->addPermanentWidget( &mOpenCvVer );
    // <<<<< OpenCV version

    // >>>>> Calibration INFO
    ui->statusBar->addWidget( &mCalibInfo );
    // <<<<< Calibration INFO

    on_pushButton_update_camera_list_clicked();

    // >>>>> Stream rendering
    mCameraSceneRaw = new QOpenCVScene();
    mCameraSceneCheckboard = new QOpenCVScene();
    mCameraSceneUndistorted = new QOpenCVScene();

    ui->graphicsView_raw->setViewport( new QGLWidget );
    ui->graphicsView_raw->setScene( mCameraSceneRaw );
    ui->graphicsView_raw->setBackgroundBrush( QBrush( QColor(200,50,50) ) );

    ui->graphicsView_checkboard->setViewport( new QGLWidget );
    ui->graphicsView_checkboard->setScene( mCameraSceneCheckboard );
    ui->graphicsView_checkboard->setBackgroundBrush( QBrush( QColor(50,200,50) ) );

    ui->graphicsView_undistorted->setViewport( new QGLWidget );
    ui->graphicsView_undistorted->setScene( mCameraSceneUndistorted );
    ui->graphicsView_undistorted->setBackgroundBrush( QBrush( QColor(50,50,200) ) );

    // <<<<< Stream rendering

    mElabPool.setMaxThreadCount( 3 );

    mIntrinsic =  cv::Mat(3, 3, CV_64F, cv::Scalar::all(0.0f) );
    mIntrinsic.ptr<double>(0)[0] = 884.0;
    mIntrinsic.ptr<double>(1)[1] = 884.0;
    mIntrinsic.ptr<double>(2)[2] = 1.0;
    mIntrinsic.ptr<double>(0)[2] = ui->lineEdit_camera_w->text().toDouble()/2.0;
    mIntrinsic.ptr<double>(1)[2] = ui->lineEdit_camera_h->text().toDouble()/2.0;

    mDistorsion = cv::Mat( 8, 1, CV_64F, cv::Scalar::all(0.0f) );
}

MainWindow::~MainWindow()
{
    killGstLaunch();

    while( mGstProcess.state() == QProcess::Running )
    {
        mGstProcess.kill();
        QApplication::processEvents( QEventLoop::AllEvents, 50 );
    }

    mElabPool.clear();

    delete ui;

    if(mCameraThread)
        delete mCameraThread;

    if(mCameraSceneRaw)
        delete mCameraSceneRaw;

    if(mCameraSceneCheckboard)
        delete mCameraSceneCheckboard;

    if(mCameraSceneUndistorted)
        delete mCameraSceneUndistorted;

    if(mCameraUndist)
        delete mCameraUndist;
}

QString MainWindow::updateOpenCvVer()
{
    QString ocvVers = tr("OpenCV %1.%2.%3").arg(CV_MAJOR_VERSION).arg(CV_MINOR_VERSION).arg(CV_SUBMINOR_VERSION);

    return ocvVers;
}

QStringList MainWindow::updateCameraInfo()
{
    QStringList res;

    mCameras = QCameraInfo::availableCameras();
    foreach (const QCameraInfo &cameraInfo, mCameras)
    {
        QString name = cameraInfo.deviceName();
        qDebug() << cameraInfo.description();

        res.push_back(name);
    }

    return res;
}

void MainWindow::on_pushButton_update_camera_list_clicked()
{
    ui->comboBox_camera->clear();
    ui->comboBox_camera->addItems( updateCameraInfo() );
}

void MainWindow::on_comboBox_camera_currentIndexChanged(int index)
{
    if( mCameras.size()<1 )
        return;

    if( index>mCameras.size()-1  )
        return;

    if( index<0 )
    {
        ui->label_camera->setText( tr("No camera info") );
    }
    else
    {
        ui->label_camera->setText( static_cast<QCameraInfo>(mCameras.at(index)).description() );
    }
}

bool MainWindow::startCamera()
{
    if(!killGstLaunch())
        return false;

    if(!startGstProcess())
        return false;

    if( mCameraThread )
    {
        delete mCameraThread;
        mCameraThread = NULL;
    }

    double fps = ui->lineEdit_camera_fps->text().toDouble();

    mCameraThread = new CameraThread( fps );

    connect( mCameraThread, &CameraThread::cameraConnected,
             this, &MainWindow::onCameraConnected );
    connect( mCameraThread, &CameraThread::cameraDisconnected,
             this, &MainWindow::onCameraDisconnected );
    connect( mCameraThread, &CameraThread::newImage,
             this, &MainWindow::onNewImage );

    mCameraThread->start();

    return true;
}

void MainWindow::stopCamera()
{
    if( mCameraThread )
    {
        disconnect( mCameraThread, &CameraThread::cameraConnected,
                    this, &MainWindow::onCameraConnected );
        disconnect( mCameraThread, &CameraThread::cameraDisconnected,
                    this, &MainWindow::onCameraDisconnected );
        disconnect( mCameraThread, &CameraThread::newImage,
                    this, &MainWindow::onNewImage );

        delete mCameraThread;
        mCameraThread = NULL;
    }
}

void MainWindow::onCameraConnected()
{
    mCameraConnected = true;
}

void MainWindow::onCameraDisconnected()
{
    mCameraConnected = false;
}

void MainWindow::onNewImage( cv::Mat frame )
{
    static int frmCnt=0;
    static int frameW = 0;
    static int frameH = 0;

    if( frameW != frame.cols ||
            frameH != frame.rows)
    {
        ui->graphicsView_raw->fitInView(QRectF(0,0, frame.cols, frame.rows),
                                        Qt::KeepAspectRatio );
        ui->graphicsView_checkboard->fitInView(QRectF(0,0, frame.cols, frame.rows),
                                               Qt::KeepAspectRatio );
        ui->graphicsView_undistorted->fitInView(QRectF(0,0, frame.cols, frame.rows),
                                                Qt::KeepAspectRatio );
        frameW = frame.cols;
        frameH = frame.rows;
    }

    mCameraSceneRaw->setFgImage(frame);

    frmCnt++;

    int fps = ui->lineEdit_camera_fps->text().toInt();

    if( ui->pushButton_calibrate->isChecked() && frmCnt%fps == 0 )
    {
        QChessboardElab* elab = new QChessboardElab( this, frame, mCbSize, mCbSizeMm, mCameraUndist );
        mElabPool.tryStart(elab);
    }

    cv::Mat rectified = mCameraUndist->undistort( frame );

    if( rectified.empty() )
    {
        mCameraSceneUndistorted->setFgImage(frame);
    }
    else
    {
        mCameraSceneUndistorted->setFgImage(rectified);
    }

    double perc = mCameraThread->getBufPerc();

    int percInt = static_cast<int>(perc*100);

    ui->progressBar_camBuffer->setValue(percInt);
}

void MainWindow::onNewCbImage(cv::Mat cbImage)
{
    mCameraSceneCheckboard->setFgImage(cbImage);

    ui->lineEdit_cb_count->setText( tr("%1").arg(mCameraUndist->getCbCount()) );
}

void MainWindow::onNewCameraParams(cv::Mat K, cv::Mat D, bool refining, double calibReprojErr)
{
    mIntrinsic = K;
    mDistorsion = D;

    if( refining )
    {
        mCalibInfo.setText( tr("Refining existing Camera parameters") );
    }
    else
    {
        mCalibInfo.setText( tr("Estimating new Camera parameters") );
    }

    ui->lineEdit_calib_reproj_err->setText(tr("%1").arg(calibReprojErr));

    if(calibReprojErr<=0.5 )
    {
        ui->lineEdit_calib_reproj_err->setStyleSheet("QLineEdit { background: rgb(50, 250, 50);}");
    }
    else if(calibReprojErr<=1.0 && calibReprojErr>0.5)
    {
        ui->lineEdit_calib_reproj_err->setStyleSheet("QLineEdit { background: rgb(250, 250, 50);}");
    }
    else
    {
        ui->lineEdit_calib_reproj_err->setStyleSheet("QLineEdit { background: rgb(250, 50, 50);}");
    }


    updateParamGUI();
}

void MainWindow::on_pushButton_camera_connect_disconnect_clicked(bool checked)
{
    if( checked )
    {
        mCamDev = ui->comboBox_camera->currentText();
        mSrcWidth = ui->lineEdit_camera_w->text().toInt();
        mSrcHeight = ui->lineEdit_camera_h->text().toInt();
        mSrcFps = ui->lineEdit_camera_fps->text().toInt();

        updateCbParams();

        if(mCameraUndist)
        {
            disconnect( mCameraUndist, &QCameraUndistort::newCameraParams,
                        this, &MainWindow::onNewCameraParams );

            delete mCameraUndist;
        }

        bool fisheye = ui->checkBox_fisheye->isChecked();

        mCameraUndist = new QCameraUndistort( cv::Size(mSrcWidth, mSrcHeight), mCbSize, mCbSizeMm, fisheye );

        connect( mCameraUndist, &QCameraUndistort::newCameraParams,
                 this, &MainWindow::onNewCameraParams );


        mCameraUndist->getCameraParams( mIntrinsic, mDistorsion );
        updateParamGUI();

        if( startCamera() )
        {
            ui->pushButton_camera_connect_disconnect->setText( tr("Stop Camera") );

            ui->lineEdit_cb_cols->setEnabled(false);
            ui->lineEdit_cb_rows->setEnabled(false);
            ui->lineEdit_cb_mm->setEnabled(false);
        }
        else
        {
            ui->pushButton_camera_connect_disconnect->setText( tr("Start Camera") );
            ui->pushButton_camera_connect_disconnect->setChecked(false);

            ui->lineEdit_cb_cols->setEnabled(true);
            ui->lineEdit_cb_rows->setEnabled(true);
            ui->lineEdit_cb_mm->setEnabled(true);
        }
    }
    else
    {
        ui->pushButton_camera_connect_disconnect->setText( tr("Start Camera") );
        stopCamera();

        ui->lineEdit_cb_cols->setEnabled(true);
        ui->lineEdit_cb_rows->setEnabled(true);
        ui->lineEdit_cb_mm->setEnabled(true);
    }
}

void MainWindow::onProcessReadyRead()
{
    while( mGstProcess.bytesAvailable() )
    {
        QByteArray line = mGstProcess.readLine();

        qDebug() << line;

        QApplication::processEvents( QEventLoop::AllEvents, 5 );
    }
}

bool MainWindow::killGstLaunch( )
{
    // >>>>> Kill gst-launch-1.0 processes
    QProcess killer;
    QProcess checker;

    int count = 0;
    bool done = false;
    do
    {
        killer.start( "pkill gst-launch" );
        killer.waitForFinished( 1000 );

        checker.start( "pgrep gst-launch" );
        checker.waitForFinished( 1000 );

        done = checker.readAll().size()==0;
        count++;

        if( count==10 )
        {
            qDebug() << tr("Cannot kill gst-launch active process(es)" );

            return false;
        }

    }
    while( !done );
    // <<<<< Kill gst-launch-1.0 processes

    return true;
}

bool MainWindow::startGstProcess( )
{
    if( mCamDev.size()==0 )
        return false;

    QString launchStr;

#ifdef USE_ARM
    launchStr = tr(
                    "gst-launch-1.0 v4l2src device=%1 do-timestamp=true ! "
                    "\"video/x-raw,format=I420,width=%2,height=%3,framerate=%4/1\" ! nvvidconv ! "
                    "\"video/x-raw(memory:NVMM),width=%2,height=%3\" ! "
                    //"omxh264enc low-latency=true insert-sps-pps=true ! "
                    "omxh264enc insert-sps-pps=true ! "
                    "rtph264pay config-interval=1 pt=96 mtu=9000 ! queue ! "
                    "udpsink host=127.0.0.1 port=5000 sync=false async=false -e"
                ).arg(mCamDev).arg(mSrcWidth).arg(mSrcHeight).arg(mSrcFps);
#else
    launchStr =
        tr("gst-launch-1.0 v4l2src device=%1 ! "
           "\"video/x-raw,format=I420,width=%2,height=%3,framerate=%4/1\" ! videoconvert ! "
           //"videoscale ! \"video/x-raw,width=%5,height=%6\" ! "
           "x264enc key-int-max=1 tune=zerolatency bitrate=8000 ! "
           "rtph264pay config-interval=1 pt=96 mtu=9000 ! queue ! "
           "udpsink host=127.0.0.1 port=5000 sync=false async=false -e").arg(mCamDev).arg(mSrcWidth).arg(mSrcHeight).arg(mSrcFps);
#endif

    qDebug() << tr("Starting pipeline: \n %1").arg(launchStr);

    mGstProcess.setProcessChannelMode( QProcess::MergedChannels );
    mGstProcess.start( launchStr );

    if( !mGstProcess.waitForStarted( 10000 ) )
    {
        // TODO Camera error message

        return false;
    }

    return true;
}

void MainWindow::updateCbParams()
{
    mCbSize.width = ui->lineEdit_cb_cols->text().toInt();
    mCbSize.height = ui->lineEdit_cb_rows->text().toInt();
    mCbSizeMm = ui->lineEdit_cb_mm->text().toFloat();
}

void MainWindow::updateParamGUI()
{
    double fx = mIntrinsic.ptr<double>(0)[0];
    double fy = mIntrinsic.ptr<double>(1)[1];
    double cx = mIntrinsic.ptr<double>(0)[2];
    double cy = mIntrinsic.ptr<double>(1)[2];
    double scale = mIntrinsic.ptr<double>(2)[2];

    ui->lineEdit_fx->setText( tr("%1").arg(fx) );
    ui->lineEdit_fy->setText( tr("%1").arg(fy) );
    ui->lineEdit_cx->setText( tr("%1").arg(cx) );
    ui->lineEdit_cy->setText( tr("%1").arg(cy) );
    ui->lineEdit_scale->setText( tr("%1").arg(scale) );

    double k1 = mDistorsion.ptr<double>(0)[0];
    double k2 = mDistorsion.ptr<double>(1)[0];

    if( ui->checkBox_fisheye->isChecked() )
    {
        double k3 = mDistorsion.ptr<double>(2)[0];
        double k4 = mDistorsion.ptr<double>(3)[0];

        ui->lineEdit_k1->setText( tr("%1").arg(k1) );
        ui->lineEdit_k2->setText( tr("%1").arg(k2) );
        ui->lineEdit_k3->setText( tr("%1").arg(k3) );
        ui->lineEdit_k4->setText( tr("%1").arg(k4) );

        ui->lineEdit_k5->setVisible(false);
        ui->lineEdit_k6->setVisible(false);
        ui->lineEdit_p1->setVisible(false);
        ui->lineEdit_p2->setVisible(false);
    }
    else
    {
        double p1 = mDistorsion.ptr<double>(2)[0];
        double p2 = mDistorsion.ptr<double>(3)[0];

        double k3 = mDistorsion.ptr<double>(4)[0];
        double k4 = mDistorsion.ptr<double>(5)[0];
        double k5 = mDistorsion.ptr<double>(6)[0];
        double k6 = mDistorsion.ptr<double>(7)[0];

        ui->lineEdit_k1->setText( tr("%1").arg(k1) );
        ui->lineEdit_k2->setText( tr("%1").arg(k2) );
        ui->lineEdit_p1->setText( tr("%1").arg(p1) );
        ui->lineEdit_p2->setText( tr("%1").arg(p2) );
        ui->lineEdit_k3->setText( tr("%1").arg(k3) );
        ui->lineEdit_k4->setText( tr("%1").arg(k4) );
        ui->lineEdit_k5->setText( tr("%1").arg(k5) );
        ui->lineEdit_k6->setText( tr("%1").arg(k6) );

        ui->lineEdit_k5->setVisible(true);
        ui->lineEdit_k6->setVisible(true);
        ui->lineEdit_p1->setVisible(true);
        ui->lineEdit_p2->setVisible(true);
    }
}

void MainWindow::setNewCameraParams()
{
    mIntrinsic.ptr<double>(0)[0] = ui->lineEdit_fx->text().toDouble();
    mIntrinsic.ptr<double>(0)[1] = ui->lineEdit_K_01->text().toDouble();
    mIntrinsic.ptr<double>(0)[2] = ui->lineEdit_cx->text().toDouble();
    mIntrinsic.ptr<double>(1)[0] = ui->lineEdit_K_10->text().toDouble();
    mIntrinsic.ptr<double>(1)[1] = ui->lineEdit_fy->text().toDouble();
    mIntrinsic.ptr<double>(1)[2] = ui->lineEdit_cy->text().toDouble();
    mIntrinsic.ptr<double>(2)[0] = ui->lineEdit_K_20->text().toDouble();
    mIntrinsic.ptr<double>(2)[1] = ui->lineEdit_K_21->text().toDouble();
    mIntrinsic.ptr<double>(2)[2] = ui->lineEdit_scale->text().toDouble();

    cout << "Intrinsic edited: " << endl << mIntrinsic << endl << endl;

    mDistorsion.ptr<double>(0)[0] = ui->lineEdit_k1->text().toDouble();
    mDistorsion.ptr<double>(1)[0] = ui->lineEdit_k2->text().toDouble();

    if(ui->checkBox_fisheye->isChecked())
    {
        mDistorsion.ptr<double>(2)[0] = ui->lineEdit_k3->text().toDouble();
        mDistorsion.ptr<double>(3)[0] = ui->lineEdit_k4->text().toDouble();
        mDistorsion.ptr<double>(4)[0] = 0.0;
        mDistorsion.ptr<double>(5)[0] = 0.0;
        mDistorsion.ptr<double>(6)[0] = 0.0;
        mDistorsion.ptr<double>(7)[0] = 0.0;
    }
    else
    {
        mDistorsion.ptr<double>(2)[0] = ui->lineEdit_p1->text().toDouble();
        mDistorsion.ptr<double>(3)[0] = ui->lineEdit_p2->text().toDouble();
        mDistorsion.ptr<double>(4)[0] = ui->lineEdit_k3->text().toDouble();
        mDistorsion.ptr<double>(5)[0] = ui->lineEdit_k4->text().toDouble();
        mDistorsion.ptr<double>(6)[0] = ui->lineEdit_k5->text().toDouble();
        mDistorsion.ptr<double>(7)[0] = ui->lineEdit_k6->text().toDouble();
    }

    cout << "Distorsion edited: " << endl << mDistorsion << endl << endl;

    if( mCameraUndist )
    {
        mCameraUndist->setCameraParams( mIntrinsic, mDistorsion, ui->checkBox_fisheye->isChecked() );
    }
}

void MainWindow::on_lineEdit_fx_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_K_01_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_cx_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_K_10_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_fy_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_cy_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_K_20_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_K_21_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_scale_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k1_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k2_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k3_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k4_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k5_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_k6_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_p1_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_lineEdit_p2_editingFinished()
{
    setNewCameraParams();
}

void MainWindow::on_pushButton_calibrate_clicked(bool checked)
{
    ui->groupBox_params->setEnabled(!checked);
}

void MainWindow::on_pushButton_load_params_clicked()
{

}

void MainWindow::on_pushButton_save_params_clicked()
{

}

void MainWindow::on_checkBox_fisheye_clicked()
{
    setNewCameraParams();

    updateParamGUI();
}
