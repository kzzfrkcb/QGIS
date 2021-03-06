/***************************************************************************
                         qgsmeshlayerinterpolator.cpp
                         ----------------------------
    begin                : April 2018
    copyright            : (C) 2018 by Peter Petrik
    email                : zilolv at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

///@cond PRIVATE

#include <memory>
#include <limits>

#include "qgsmeshlayerinterpolator.h"

#include "qgis.h"
#include "qgsrasterinterface.h"
#include "qgsmaptopixel.h"
#include "qgsvector.h"
#include "qgspoint.h"
#include "qgspointxy.h"
#include "qgsmeshlayerutils.h"
#include "qgsmeshlayer.h"
#include "qgscoordinatetransformcontext.h"
#include "qgscoordinatetransform.h"
#include "qgsmeshdataprovider.h"

QgsMeshLayerInterpolator::QgsMeshLayerInterpolator( const QgsTriangularMesh &m,
    const QVector<double> &datasetValues, const QgsMeshDataBlock &activeFaceFlagValues,
    bool dataIsOnVertices,
    const QgsRenderContext &context,
    const QSize &size )
  : mTriangularMesh( m ),
    mDatasetValues( datasetValues ),
    mActiveFaceFlagValues( activeFaceFlagValues ),
    mContext( context ),
    mDataOnVertices( dataIsOnVertices ),
    mOutputSize( size )
{
}

QgsMeshLayerInterpolator::~QgsMeshLayerInterpolator() = default;

QgsRasterInterface *QgsMeshLayerInterpolator::clone() const
{
  assert( false ); // we should not need this (hopefully)
  return nullptr;
}

Qgis::DataType QgsMeshLayerInterpolator::dataType( int ) const
{
  return Qgis::Float64;
}

int QgsMeshLayerInterpolator::bandCount() const
{
  return 1;
}

QgsRasterBlock *QgsMeshLayerInterpolator::block( int, const QgsRectangle &extent, int width, int height, QgsRasterBlockFeedback *feedback )
{
  std::unique_ptr<QgsRasterBlock> outputBlock( new QgsRasterBlock( Qgis::Float64, width, height ) );
  const double noDataValue = std::numeric_limits<double>::quiet_NaN();
  outputBlock->setNoDataValue( noDataValue );
  outputBlock->setIsNoData();  // assume initially that all values are unset
  double *data = reinterpret_cast<double *>( outputBlock->bits() );

  const QVector<QgsMeshFace> &triangles = mTriangularMesh.triangles();
  const QVector<QgsMeshVertex> &vertices = mTriangularMesh.vertices();

  // currently expecting that triangulation does not add any new extra vertices on the way
  if ( mDataOnVertices )
    Q_ASSERT( mDatasetValues.count() == mTriangularMesh.vertices().count() );

  for ( int i = 0; i < triangles.size(); ++i )
  {
    if ( feedback && feedback->isCanceled() )
      break;

    if ( mContext.renderingStopped() )
      break;

    const QgsMeshFace &face = triangles[i];

    const int v1 = face[0], v2 = face[1], v3 = face[2];
    const QgsPoint p1 = vertices[v1], p2 = vertices[v2], p3 = vertices[v3];

    const int nativeFaceIndex = mTriangularMesh.trianglesToNativeFaces()[i];
    const bool isActive = mActiveFaceFlagValues.active( nativeFaceIndex );
    if ( !isActive )
      continue;

    QgsRectangle bbox = QgsMeshLayerUtils::triangleBoundingBox( p1, p2, p3 );
    if ( !extent.intersects( bbox ) )
      continue;

    // Get the BBox of the element in pixels
    int topLim, bottomLim, leftLim, rightLim;
    QgsMeshLayerUtils::boundingBoxToScreenRectangle( mContext.mapToPixel(), mOutputSize, bbox, leftLim, rightLim, topLim, bottomLim );

    // interpolate in the bounding box of the face
    for ( int j = topLim; j <= bottomLim; j++ )
    {
      double *line = data + ( j * width );
      for ( int k = leftLim; k <= rightLim; k++ )
      {
        double val;
        const QgsPointXY p = mContext.mapToPixel().toMapCoordinates( k, j );
        if ( mDataOnVertices )
          val = QgsMeshLayerUtils::interpolateFromVerticesData(
                  p1,
                  p2,
                  p3,
                  mDatasetValues[v1],
                  mDatasetValues[v2],
                  mDatasetValues[v3],
                  p );
        else
        {
          int face = mTriangularMesh.trianglesToNativeFaces()[i];
          val = QgsMeshLayerUtils::interpolateFromFacesData(
                  p1,
                  p2,
                  p3,
                  mDatasetValues[face],
                  p
                );
        }

        if ( !std::isnan( val ) )
        {
          line[k] = val;
          outputBlock->setIsData( j, k );
        }
      }
    }

  }

  return outputBlock.release();
}

///@endcond

QgsRasterBlock *QgsMeshUtils::exportRasterBlock(
  const QgsMeshLayer &layer,
  const QgsMeshDatasetIndex &datasetIndex,
  const QgsCoordinateReferenceSystem &destinationCrs,
  const QgsCoordinateTransformContext &transformContext,
  double mapUnitsPerPixel,
  const QgsRectangle &extent,
  QgsRasterBlockFeedback *feedback )
{
  if ( !layer.dataProvider() )
    return nullptr;

  if ( !datasetIndex.isValid() )
    return nullptr;

  int widthPixel = static_cast<int>( extent.width() / mapUnitsPerPixel );
  int heightPixel = static_cast<int>( extent.height() / mapUnitsPerPixel );

  const QgsPointXY center = extent.center();
  QgsMapToPixel mapToPixel( mapUnitsPerPixel,
                            center.x(),
                            center.y(),
                            widthPixel,
                            heightPixel,
                            0 );
  QgsCoordinateTransform transform( layer.crs(), destinationCrs, transformContext );

  QgsRenderContext renderContext;
  renderContext.setCoordinateTransform( transform );
  renderContext.setMapToPixel( mapToPixel );
  renderContext.setExtent( extent );

  std::unique_ptr<QgsMesh> nativeMesh = qgis::make_unique<QgsMesh>();
  layer.dataProvider()->populateMesh( nativeMesh.get() );
  std::unique_ptr<QgsTriangularMesh> triangularMesh = qgis::make_unique<QgsTriangularMesh>();
  triangularMesh->update( nativeMesh.get(), &renderContext );

  const QgsMeshDatasetGroupMetadata metadata = layer.dataProvider()->datasetGroupMetadata( datasetIndex );
  bool scalarDataOnVertices = metadata.dataType() == QgsMeshDatasetGroupMetadata::DataOnVertices;

  QgsMeshDataBlock vals = layer.dataProvider()->datasetValues(
                            datasetIndex,
                            0,
                            scalarDataOnVertices ? nativeMesh->vertices.count() : nativeMesh->faces.count() );

  QVector<double> datasetValues = QgsMeshLayerUtils::calculateMagnitudes( vals );
  QgsMeshDataBlock activeFaceFlagValues = layer.dataProvider()->areFacesActive(
      datasetIndex,
      0,
      nativeMesh->faces.count() );

  QgsMeshLayerInterpolator iterpolator(
    *( triangularMesh.get() ),
    datasetValues,
    activeFaceFlagValues,
    scalarDataOnVertices,
    renderContext,
    QSize( widthPixel, heightPixel )
  );

  return iterpolator.block( 0, extent, widthPixel, heightPixel, feedback );
}
