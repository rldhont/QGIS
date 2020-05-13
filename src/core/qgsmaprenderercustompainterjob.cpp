/***************************************************************************
  qgsmaprenderercustompainterjob.cpp
  --------------------------------------
  Date                 : December 2013
  Copyright            : (C) 2013 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsmaprenderercustompainterjob.h"

#include "qgsfeedback.h"
#include "qgslabelingengine.h"
#include "qgslogger.h"
#include "qgsmaplayerrenderer.h"
#include "qgsmaplayerlistutils.h"

#include <QtConcurrentRun>

//
// QgsMapRendererAbstractCustomPainterJob
//

QgsMapRendererAbstractCustomPainterJob::QgsMapRendererAbstractCustomPainterJob( const QgsMapSettings &settings )
  : QgsMapRendererJob( settings )
{

}

void QgsMapRendererAbstractCustomPainterJob::preparePainter( QPainter *painter, const QColor &backgroundColor )
{
  // clear the background
  painter->fillRect( 0, 0, mSettings.deviceOutputSize().width(), mSettings.deviceOutputSize().height(), backgroundColor );

  painter->setRenderHint( QPainter::Antialiasing, mSettings.testFlag( QgsMapSettings::Antialiasing ) );

#ifndef QT_NO_DEBUG
  QPaintDevice *paintDevice = painter->device();
  QString errMsg = QStringLiteral( "pre-set DPI not equal to painter's DPI (%1 vs %2)" )
                   .arg( paintDevice->logicalDpiX() )
                   .arg( mSettings.outputDpi() * mSettings.devicePixelRatio() );
  Q_ASSERT_X( qgsDoubleNear( paintDevice->logicalDpiX(), mSettings.outputDpi() * mSettings.devicePixelRatio(), 1.0 ),
              "Job::startRender()", errMsg.toLatin1().data() );
#endif
}


//
// QgsMapRendererCustomPainterJob
//

QgsMapRendererCustomPainterJob::QgsMapRendererCustomPainterJob( const QgsMapSettings &settings, QPainter *painter )
  : QgsMapRendererAbstractCustomPainterJob( settings )
  , mPainter( painter )
  , mActive( false )
  , mRenderSynchronously( false )
{
  QgsDebugMsg( QStringLiteral( "QPAINTER construct" ) );
}

QgsMapRendererCustomPainterJob::~QgsMapRendererCustomPainterJob()
{
  QgsDebugMsg( QStringLiteral( "QPAINTER destruct" ) );
  Q_ASSERT( !mFutureWatcher.isRunning() );
  //cancel();
}

void QgsMapRendererCustomPainterJob::start()
{
  if ( isActive() )
    return;

  if ( !mPrepareOnly )
    mRenderingStart.start();

  mActive = true;

  mErrors.clear();

  QgsDebugMsg( QStringLiteral( "QPAINTER run!" ) );

  QgsDebugMsg( QStringLiteral( "Preparing list of layer jobs for rendering" ) );
  QTime prepareTime;
  prepareTime.start();

  preparePainter( mPainter, mSettings.backgroundColor() );

  mLabelingEngineV2.reset();

  if ( mSettings.testFlag( QgsMapSettings::DrawLabeling ) )
  {
    mLabelingEngineV2.reset( new QgsDefaultLabelingEngine() );
    mLabelingEngineV2->setMapSettings( mSettings );
  }

  bool canUseLabelCache = prepareLabelCache();
  mLayerJobs = prepareJobs( mPainter, mLabelingEngineV2.get() );
  mLabelJob = prepareLabelingJob( mPainter, mLabelingEngineV2.get(), canUseLabelCache );

  QgsDebugMsg( QStringLiteral( "Rendering prepared in (seconds): %1" ).arg( prepareTime.elapsed() / 1000.0 ) );

  if ( mRenderSynchronously )
  {
    if ( !mPrepareOnly )
    {
      // do the rendering right now!
      doRender();
    }
    return;
  }

  // now we are ready to start rendering!
  connect( &mFutureWatcher, &QFutureWatcher<void>::finished, this, &QgsMapRendererCustomPainterJob::futureFinished );

  mFuture = QtConcurrent::run( staticRender, this );
  mFutureWatcher.setFuture( mFuture );
}


void QgsMapRendererCustomPainterJob::cancel()
{
  if ( !isActive() )
  {
    QgsDebugMsg( QStringLiteral( "QPAINTER not running!" ) );
    return;
  }

  QgsDebugMsg( QStringLiteral( "QPAINTER canceling" ) );
  disconnect( &mFutureWatcher, &QFutureWatcher<void>::finished, this, &QgsMapRendererCustomPainterJob::futureFinished );
  cancelWithoutBlocking();

  QTime t;
  t.start();

  mFutureWatcher.waitForFinished();

  QgsDebugMsg( QStringLiteral( "QPAINER cancel waited %1 ms" ).arg( t.elapsed() / 1000.0 ) );

  futureFinished();

  QgsDebugMsg( QStringLiteral( "QPAINTER canceled" ) );
}

void QgsMapRendererCustomPainterJob::cancelWithoutBlocking()
{
  if ( !isActive() )
  {
    QgsDebugMsg( QStringLiteral( "QPAINTER not running!" ) );
    return;
  }

  mLabelJob.context.setRenderingStopped( true );
  for ( LayerRenderJobs::iterator it = mLayerJobs.begin(); it != mLayerJobs.end(); ++it )
  {
    it->context.setRenderingStopped( true );
    if ( it->renderer && it->renderer->feedback() )
      it->renderer->feedback()->cancel();
  }
}

void QgsMapRendererCustomPainterJob::waitForFinished()
{
  if ( !isActive() )
    return;

  disconnect( &mFutureWatcher, &QFutureWatcher<void>::finished, this, &QgsMapRendererCustomPainterJob::futureFinished );

  QTime t;
  t.start();

  mFutureWatcher.waitForFinished();

  QgsDebugMsg( QStringLiteral( "waitForFinished: %1 ms" ).arg( t.elapsed() / 1000.0 ) );

  futureFinished();
}

bool QgsMapRendererCustomPainterJob::isActive() const
{
  return mActive;
}

bool QgsMapRendererCustomPainterJob::usedCachedLabels() const
{
  return mLabelJob.cached;
}

QgsLabelingResults *QgsMapRendererCustomPainterJob::takeLabelingResults()
{
  if ( mLabelingEngineV2 )
    return mLabelingEngineV2->takeResults();
  else
    return nullptr;
}


void QgsMapRendererCustomPainterJob::waitForFinishedWithEventLoop( QEventLoop::ProcessEventsFlags flags )
{
  QEventLoop loop;
  connect( &mFutureWatcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit );
  loop.exec( flags );
}


void QgsMapRendererCustomPainterJob::renderSynchronously()
{
  mRenderSynchronously = true;
  start();
  futureFinished();
  mRenderSynchronously = false;
}

void QgsMapRendererCustomPainterJob::prepare()
{
  mRenderSynchronously = true;
  mPrepareOnly = true;
  start();
  mPrepared = true;
}

void QgsMapRendererCustomPainterJob::renderPrepared()
{
  if ( !mPrepared )
    return;

  doRender();
  futureFinished();
  mRenderSynchronously = false;
  mPrepareOnly = false;
  mPrepared = false;
}

void QgsMapRendererCustomPainterJob::futureFinished()
{
  mActive = false;
  if ( !mPrepared ) // can't access from other thread
    mRenderingTime = mRenderingStart.elapsed();
  QgsDebugMsg( QStringLiteral( "QPAINTER futureFinished" ) );

  if ( !mPrepared )
    logRenderingTime( mLayerJobs, mLabelJob );

  // final cleanup
  cleanupJobs( mLayerJobs );
  cleanupLabelJob( mLabelJob );

  emit finished();
}


void QgsMapRendererCustomPainterJob::staticRender( QgsMapRendererCustomPainterJob *self )
{
  try
  {
    self->doRender();
  }
  catch ( QgsException &e )
  {
    Q_UNUSED( e )
    QgsDebugMsg( "Caught unhandled QgsException: " + e.what() );
  }
  catch ( std::exception &e )
  {
    Q_UNUSED( e )
    QgsDebugMsg( "Caught unhandled std::exception: " + QString::fromLatin1( e.what() ) );
  }
  catch ( ... )
  {
    QgsDebugMsg( QStringLiteral( "Caught unhandled unknown exception" ) );
  }
}

void QgsMapRendererCustomPainterJob::doRender()
{
  QgsDebugMsg( QStringLiteral( "Starting to render layer stack." ) );
  QTime renderTime;
  renderTime.start();

  for ( LayerRenderJobs::iterator it = mLayerJobs.begin(); it != mLayerJobs.end(); ++it )
  {
    LayerRenderJob &job = *it;

    if ( job.context.renderingStopped() )
      break;

    if ( job.context.useAdvancedEffects() )
    {
      // Set the QPainter composition mode so that this layer is rendered using
      // the desired blending mode
      mPainter->setCompositionMode( job.blendMode );
    }

    if ( !job.cached )
    {
      QTime layerTime;
      layerTime.start();

      if ( job.img )
      {
        job.img->fill( 0 );
        job.imageInitialized = true;
      }

      job.renderer->render();

      job.renderingTime += layerTime.elapsed();
    }

    if ( job.img )
    {
      // If we flattened this layer for alternate blend modes, composite it now
      mPainter->setOpacity( job.opacity );
      mPainter->drawImage( 0, 0, *job.img );
      mPainter->setOpacity( 1.0 );
    }

  }

  QgsDebugMsg( QStringLiteral( "Done rendering map layers" ) );

  if ( mSettings.testFlag( QgsMapSettings::DrawLabeling ) && !mLabelJob.context.renderingStopped() )
  {
    if ( !mLabelJob.cached )
    {
      QTime labelTime;
      labelTime.start();

      if ( mLabelJob.img )
      {
        QPainter painter;
        mLabelJob.img->fill( 0 );
        painter.begin( mLabelJob.img );
        mLabelJob.context.setPainter( &painter );
        drawLabeling( mLabelJob.context, mLabelingEngineV2.get(), &painter );
        painter.end();
      }
      else
      {
        drawLabeling( mLabelJob.context, mLabelingEngineV2.get(), mPainter );
      }

      mLabelJob.complete = true;
      mLabelJob.renderingTime = labelTime.elapsed();
      mLabelJob.participatingLayers = _qgis_listRawToQPointer( mLabelingEngineV2->participatingLayers() );
    }
  }
  if ( mLabelJob.img && mLabelJob.complete )
  {
    mPainter->setCompositionMode( QPainter::CompositionMode_SourceOver );
    mPainter->setOpacity( 1.0 );
    mPainter->drawImage( 0, 0, *mLabelJob.img );
  }

  QgsDebugMsg( QStringLiteral( "Rendering completed in (seconds): %1" ).arg( renderTime.elapsed() / 1000.0 ) );
}


