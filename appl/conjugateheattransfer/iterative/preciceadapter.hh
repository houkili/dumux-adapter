#ifndef PRECICEWRAPPER_HH
#define PRECICEWRAPPER_HH

#include<string>
#include<ostream>
#include<precice/SolverInterface.hpp>

#include "dumuxpreciceindexwrapper.hh"

namespace precice_adapter{

class PreciceAdapter
{

private:
  bool wasCreated_;
  std::unique_ptr<precice::SolverInterface> precice_;

  PreciceAdapter();

  bool checkIfActionIsRequired( const std::string& condition );
  void actionIsFulfilled( const std::string& condition );

  void readBlockScalarDataFromPrecice( const int dataID, std::vector<double>& data );
  void writeBlockScalarDataToPrecice( const int dataID, std::vector<double>& data );

  bool meshWasCreated_;
  bool preciceWasInitialized_;
  int meshID_;
//  int dimension_;
  int heatFluxID_;
  int temperatureID_;

  double timeStepSize_;

  std::vector<int> vertexIDs_; //should be size_t
  std::vector<double> heatFlux_;
  std::vector<double> temperature_;

  DumuxPreciceIndexMapper<int> indexMapper_;

//  DumuxSolutionType solutionCheckpoint_;

  double initialize();

  ~PreciceAdapter();
public:
  PreciceAdapter(const PreciceAdapter&) = delete;
  void operator=(const PreciceAdapter&) = delete;

  static PreciceAdapter& getInstance();

  void announceSolver( const std::string& name,
                       const std::string& configurationFileName,
                       const int rank,
                       const int size );
//  void configure( const std::string& configurationFileName );

  /*
  void announceHeatFluxToWrite( const HeatFluxType heatFluxType );
  void announceHeatFluxToRead( const HeatFluxType heatFluxType );
  */

  int getDimensions();
  // static int getMeshID( const std::string& meshName );
  //static void setMeshName( const std::string& meshName );
  //static int getDataID( const std::string& dataName, const int meshID );

  //static void writeInitialBlockScalarData( const int dataID,
  //                                         const int size,
  //                                         int* const valueIndices,
  //                                         double* const values );
  //static void announceAllInitialDataWritten();

  bool hasToReadIterationCheckpoint();
  void announceIterationCheckpointRead();
  bool hasToWriteIterationCheckpoint();
  void announceIterationCheckpointWritten();

  bool hasToWriteInitialData();
  void announceInitialDataWritten();

  bool isInitialDataAvailable();

  double setMeshAndInitialize( const std::string& meshName,
                             const size_t numPoints,
                             std::vector<double>& coordinates,
                             const std::vector<int>& dumuxFaceIDs ) ;

  void initializeData();
  //static void initializeData();

  double advance( const double computedTimeStepLength );
  bool isCouplingOngoing();

  size_t getNumberOfVertices();


  double getHeatFluxOnFace( const int faceID ) const;
  void writeHeatFluxOnFace( const int faceID, const double value );

  double getTemperatureOnFace( const int faceID ) const;
  void writeTemperatureOnFace( const int faceID, const double value );

  void writeHeatFluxToOtherSolver();
  void readHeatFluxFromOtherSolver();

  void writeTemperatureToOtherSolver();
  void readTemperatureFromOtherSolver();


  bool isCoupledEntity( const int faceID ) const;

  void print( std::ostream& os );

  void finalize();

};

}
#endif
