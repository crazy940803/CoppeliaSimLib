#include "simInternal.h"
#include "luaScriptFunctions.h"
#include "tt.h"
#include "threadPool.h"
#include "linMotionRoutines.h"
#include "pluginContainer.h"
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include "vVarious.h"
#include "vDateTime.h"
#include "app.h"
#include "apiErrors.h"
#include "interfaceStack.h"
#include "fileOperations.h"
#include "ttUtil.h"
#include "imgLoaderSaver.h"
#include "sceneObjectOperations.h"
#include "collisionRoutines.h"
#include "distanceRoutines.h"

#define LUA_START(funcName) \
    CApiErrors::clearThreadBasedFirstCapiErrorAndWarning(); \
    std::string functionName(funcName); \
    std::string errorString; \
    std::string warningString; \
    bool cSideErrorOrWarningReporting=true;

#define LUA_START_NO_CSIDE_ERROR(funcName) \
    CApiErrors::clearThreadBasedFirstCapiErrorAndWarning(); \
    std::string functionName(funcName); \
    std::string errorString; \
    std::string warningString; \
    bool cSideErrorOrWarningReporting=false;

#define LUA_END(p) \
    if (true) \
    { \
        _reportWarningsIfNeeded(L,functionName.c_str(),warningString.c_str(),cSideErrorOrWarningReporting); \
        return(p); \
    } \
    else

void _reportWarningsIfNeeded(luaWrap_lua_State* L,const char* functionName,const char* warningString,bool cSideErrorOrWarningReporting)
{
    std::string warnStr(warningString);
    if ( (warnStr.size()==0)&&cSideErrorOrWarningReporting )
        warnStr=CApiErrors::getAndClearThreadBasedFirstCapiWarning();
    if (warnStr.size()>0)
    {
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
        if (it!=nullptr)
        {
            int verb=sim_verbosity_scriptwarnings;
            int lineNumber=-1;
            lineNumber=luaWrap_getCurrentCodeLine(L);
            std::string msg;
            msg+=std::to_string(lineNumber);
            msg+=": ";
            msg+=warnStr;
            msg+=" (in function '";
            msg+=functionName;
            msg+="')";
            if (App::userSettings->undecoratedStatusbarMessages)
                it->prefixWithLuaLocationName(msg);
            App::logScriptMsg(it->getShortDescriptiveName().c_str(),verb,msg.c_str());
        }
    }
}

void _raiseErrorOrYieldIfNeeded(luaWrap_lua_State* L,const char* functionName,const char* errorString,bool cSideErrorOrWarningReporting)
{
    if (strcmp(errorString,"@yield")!=0)
    {
        std::string errStr(errorString);
        if ( (errStr.size()==0)&&cSideErrorOrWarningReporting )
            errStr=CApiErrors::getAndClearThreadBasedFirstCapiError();
        if (errStr.size()==0)
            return;
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
        if (it==nullptr)
            return;
        it->setLastError(errStr.c_str());
        if (!it->getRaiseErrors_backCompatibility())
            return;
        int lineNumber=-1;
        lineNumber=luaWrap_getCurrentCodeLine(L);
        std::string msg;
        msg+=std::to_string(lineNumber);
        msg+=": ";
        msg+=errStr;
        msg+=" (in function '";
        msg+=functionName;
        msg+="')";
        it->prefixWithLuaLocationName(msg);
        luaWrap_lua_pushstring(L,msg.c_str());
    }
    if (strcmp(errorString,"@yield")==0)
        luaWrap_lua_yield(L,0); // does a long jump and never returns
    luaWrap_lua_error(L); // does a long jump and never returns
}

#define LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED() _raiseErrorOrYieldIfNeeded(L,functionName.c_str(),errorString.c_str(),cSideErrorOrWarningReporting)
#define SIM_SCRIPT_NAME_INDEX "sim_script_name_index" // keep this global, e.g. not __HIDDEN__.sim_script_name_index

std::vector<int> serialPortHandles;
std::vector<std::string> serialPortLeftOverData;

const SLuaCommands simLuaCommands[]=
{
    {"sim.getScriptName",_simGetScriptName,                      "string scriptName=sim.getScriptName(number scriptHandle)",true},
    {"sim.getObjectAssociatedWithScript",_simGetObjectAssociatedWithScript,"number objectHandle=sim.getObjectAssociatedWithScript(number scriptHandle)",true},
    {"sim.getScriptAssociatedWithObject",_simGetScriptAssociatedWithObject,"number scriptHandle=sim.getScriptAssociatedWithObject(number objectHandle)",true},
    {"sim.getCustomizationScriptAssociatedWithObject",_simGetCustomizationScriptAssociatedWithObject,"number scriptHandle=sim.getCustomizationScriptAssociatedWithObject(number objectHandle)",true},
    {"sim.openModule",_simOpenModule,                            "number result=sim.openModule(sim.handle_all)\nnumber result=sim.openModule(string moduleName)",true},
    {"sim.closeModule",_simCloseModule,                          "number result=sim.closeModule(sim.handle_all)\nnumber result=sim.closeModule(string moduleName)",true},
    {"sim.handleModule",_simHandleModule,                        "number result=sim.handleModule(sim.handle_all)\nnumber result=sim.handleModule(string moduleName)",true},
    {"sim.boolOr32",_simBoolOr32,                                "number result=sim.boolOr32(number value1,number value2)",true},
    {"sim.boolAnd32",_simBoolAnd32,                              "number result=sim.boolAnd32(number value1,number value2)",true},
    {"sim.boolXor32",_simBoolXor32,                              "number result=sim.boolXor32(number value1,number value2)",true},
    {"sim.handleDynamics",_simHandleDynamics,                    "number result=sim.handleDynamics(number deltaTime)",true},
    {"sim.handleCollision",_simHandleCollision,                  "number collisionCount,table_2 collidingObjectHandles=sim.handleCollision(number collisionObjectHandle)",true},
    {"sim.readCollision",_simReadCollision,                      "number collisionState,table_2 collidingObjectHandles=sim.readCollision(number collisionObjectHandle)",true},
    {"sim.handleDistance",_simHandleDistance,                    "number result,number smallestDistance=sim.handleDistance(number distanceObjectHandle)",true},
    {"sim.readDistance",_simReadDistance,                        "number result,number smallestDistance=sim.readDistance(number distanceObjectHandle)",true},
    {"sim.handleProximitySensor",_simHandleProximitySensor,      "number result,number distance,table_3 detectedPoint,number detectedObjectHandle,table_3 normalVector=\nsim.handleProximitySensor(number sensorHandle)",true},
    {"sim.readProximitySensor",_simReadProximitySensor,          "number result,number distance,table_3 detectedPoint,number detectedObjectHandle,table_3 normalVector=\nsim.readProximitySensor(number sensorHandle)",true},
    {"sim.resetCollision",_simResetCollision,                    "sim.resetCollision(number collisionObjectHandle)",true},
    {"sim.resetDistance",_simResetDistance,                      "sim.resetDistance(number distanceObjectHandle)",true},
    {"sim.resetProximitySensor",_simResetProximitySensor,        "sim.resetProximitySensor(number objectHandle)",true},
    {"sim.checkProximitySensor",_simCheckProximitySensor,        "number result,number distance,table_3 detectedPoint=sim.checkProximitySensor(number sensorHandle,number entityHandle)",true},
    {"sim.checkProximitySensorEx",_simCheckProximitySensorEx,    "number result,number distance,table_3 detectedPoint,number detectedObjectHandle,table_3 normalVector=\nsim.checkProximitySensorEx(number sensorHandle,number entityHandle,number mode,number threshold,number maxAngle)",true},
    {"sim.checkProximitySensorEx2",_simCheckProximitySensorEx2,  "number result,number distance,table_3 detectedPoint,table_3 normalVector=\nsim.checkProximitySensorEx2(number sensorHandle,table vertices,number itemType,number itemCount,number mode,number threshold,number maxAngle)",true},
    {"sim.getObjectHandle",_simGetObjectHandle,                  "number objectHandle=sim.getObjectHandle(string objectName)",true},
    {"sim.addScript",_simAddScript,                              "number scriptHandle=sim.addScript(number scriptType)",true},
    {"sim.associateScriptWithObject",_simAssociateScriptWithObject,"sim.associateScriptWithObject(number scriptHandle,number objectHandle)",true},
    {"sim.setScriptText",_simSetScriptText,                      "sim.setScriptText(number scriptHandle,string scriptText)",true},
    {"sim.getScriptHandle",_simGetScriptHandle,                  "number scriptHandle=sim.getScriptHandle(string scriptName=nil)",true},
    {"sim.getCollectionHandle",_simGetCollectionHandle,          "number collectionHandle=sim.getCollectionHandle(string collectionName)",true},
    {"sim.removeCollection",_simRemoveCollection,                "sim.removeCollection(number collectionHandle)",true},
    {"sim.emptyCollection",_simEmptyCollection,                  "sim.emptyCollection(number collectionHandle)",true},
    {"sim.getObjectPosition",_simGetObjectPosition,              "table_3 position=sim.getObjectPosition(number objectHandle,number relativeToObjectHandle)",true},
    {"sim.getObjectOrientation",_simGetObjectOrientation,        "table_3 eulerAngles=sim.getObjectOrientation(number objectHandle,number relativeToObjectHandle)",true},
    {"sim.setObjectPosition",_simSetObjectPosition,              "sim.setObjectPosition(number objectHandle,number relativeToObjectHandle,table_3 position)",true},
    {"sim.setObjectOrientation",_simSetObjectOrientation,        "sim.setObjectOrientation(number objectHandle,number relativeToObjectHandle,table_3 eulerAngles)",true},
    {"sim.getJointPosition",_simGetJointPosition,                "number position=sim.getJointPosition(number objectHandle)",true},
    {"sim.setJointPosition",_simSetJointPosition,                "sim.setJointPosition(number objectHandle,number position)",true},
    {"sim.setJointTargetPosition",_simSetJointTargetPosition,    "sim.setJointTargetPosition(number objectHandle,number targetPosition)",true},
    {"sim.getJointTargetPosition",_simGetJointTargetPosition,    "number result,number targetPosition=sim.getJointTargetPosition(number objectHandle)",true},
    {"sim.setJointMaxForce",_simSetJointMaxForce,                "sim.setJointMaxForce(number objectHandle,number forceOrTorque)",true},
    {"sim.setJointTargetVelocity",_simSetJointTargetVelocity,    "sim.setJointTargetVelocity(number objectHandle,number targetVelocity)",true},
    {"sim.getJointTargetVelocity",_simGetJointTargetVelocity,    "number targetVelocity=sim.getJointTargetVelocity(number objectHandle)",true},
    {"sim.getObjectName",_simGetObjectName,                      "string objectName=sim.getObjectName(number objectHandle)",true},
    {"sim.getCollectionName",_simGetCollectionName,              "string collectionName=sim.getCollectionName(number collectionHandle)",true},
    {"sim.removeObject",_simRemoveObject,                        "number result=sim.removeObject(number objectHandle)",true},
    {"sim.removeModel",_simRemoveModel,                          "number removedObjects=sim.removeModel(number objectHandle)",true},
    {"sim.getSimulationTime",_simGetSimulationTime,              "number simulationTime=sim.getSimulationTime()",true},
    {"sim.getSimulationState",_simGetSimulationState,            "number simulationState=sim.getSimulationState()",true},
    {"sim.getSystemTime",_simGetSystemTime,                      "number systemTime=sim.getSystemTime()",true},
    {"sim.getSystemTimeInMs",_simGetSystemTimeInMs,              "number systemTimeOrTimeDiff=sim.getSystemTimeInMs(number previousTime)",true},
    {"sim.checkCollision",_simCheckCollision,                    "number result,table_2 collidingObjects=sim.checkCollision(number entity1Handle,number entity2Handle)",true},
    {"sim.checkCollisionEx",_simCheckCollisionEx,                "number segmentCount,table segmentData=sim.checkCollisionEx(number entity1Handle,number entity2Handle)",true},
    {"sim.checkDistance",_simCheckDistance,                      "number result,table_7 distanceData,table_2 objectHandlePair=sim.checkDistance(number entity1Handle,number entity2Handle,number threshold=0)",true},
    {"sim.getObjectConfiguration",_simGetObjectConfiguration,    "number rawBufferHandle=sim.getObjectConfiguration(number objectHandle)",true},
    {"sim.setObjectConfiguration",_simSetObjectConfiguration,    "sim.setObjectConfiguration(number rawBufferHandle)",true},
    {"sim.getConfigurationTree",_simGetConfigurationTree,        "number rawBufferHandle=sim.getConfigurationTree(number objectHandle)",true},
    {"sim.setConfigurationTree",_simSetConfigurationTree,        "sim.setConfigurationTree(number rawBufferHandle)",true},
    {"sim.getSimulationTimeStep",_simGetSimulationTimeStep,      "number timeStep=sim.getSimulationTimeStep()",true},
    {"sim.getSimulatorMessage",_simGetSimulatorMessage,          "number messageID,table_4 auxiliaryData,table auxiliaryData2=sim.getSimulatorMessage()",true},
    {"sim.resetGraph",_simResetGraph,                            "sim.resetGraph(number objectHandle)",true},
    {"sim.handleGraph",_simHandleGraph,                          "sim.handleGraph(number objectHandle,number simulationTime)",true},
    {"sim.getLastError",_simGetLastError,                        "string lastError=sim.getLastError([scriptHandle])",true},
    {"sim.getObjects",_simGetObjects,                            "number objectHandle=sim.getObjects(number index,number objectType)",true},
    {"sim.refreshDialogs",_simRefreshDialogs,                    "number result=sim.refreshDialogs(number refreshDegree)",true},
    {"sim.getModuleName",_simGetModuleName,                      "string moduleName,number version=sim.getModuleName(number index)",true},
    {"sim.removeScript",_simRemoveScript,                        "sim.removeScript(number scriptHandle)",true},
    {"sim.getCollisionHandle",_simGetCollisionHandle,            "number collisionObjectHandle=sim.getCollisionHandle(string collisionObjectName)",true},
    {"sim.getDistanceHandle",_simGetDistanceHandle,              "number distanceObjectHandle=sim.getDistanceHandle(string distanceObjectName)",true},
    {"sim.stopSimulation",_simStopSimulation,                    "number result=sim.stopSimulation()",true},
    {"sim.pauseSimulation",_simPauseSimulation,                  "number result=sim.pauseSimulation()",true},
    {"sim.startSimulation",_simStartSimulation,                  "number result=sim.startSimulation()",true},
    {"sim.getObjectMatrix",_simGetObjectMatrix,                  "table_12 matrix=sim.getObjectMatrix(number objectHandle,number relativeToObjectHandle)",true},
    {"sim.setObjectMatrix",_simSetObjectMatrix,                  "sim.setObjectMatrix(number objectHandle,number relativeToObjectHandle,table_12 matrix)",true},
    {"sim.getJointMatrix",_simGetJointMatrix,                    "table_12 matrix=sim.getJointMatrix(number objectHandle)",true},
    {"sim.setSphericalJointMatrix",_simSetSphericalJointMatrix,  "sim.setSphericalJointMatrix(number objectHandle,table_12 matrix)",true},
    {"sim.buildIdentityMatrix",_simBuildIdentityMatrix,          "table_12 matrix=sim.buildIdentityMatrix()",true},
    {"sim.copyMatrix",_simCopyMatrix,                            "table_12 matrix=sim.copyMatrix(table_12 matrixToCopy)",true},
    {"sim.buildMatrix",_simBuildMatrix,                          "table_12 matrix=sim.buildMatrix(table_3 position,table_3 eulerAngles)",true},
    {"sim.getEulerAnglesFromMatrix",_simGetEulerAnglesFromMatrix,"table_3 eulerAngles=sim.getEulerAnglesFromMatrix(table_12 matrix)",true},
    {"sim.invertMatrix",_simInvertMatrix,                        "sim.invertMatrix(table_12 matrix)",true},
    {"sim.multiplyMatrices",_simMultiplyMatrices,                "table_12 resultMatrix=sim.multiplyMatrices(table_12 matrixIn1,table_12 matrixIn2)",true},
    {"sim.interpolateMatrices",_simInterpolateMatrices,          "table_12 resultMatrix=sim.interpolateMatrices(table_12 matrixIn1,table_12 matrixIn2,number interpolFactor)",true},
    {"sim.multiplyVector",_simMultiplyVector,                    "table_3n resultVector=sim.multiplyVector(table_12 matrix,table_3n vector)",true},
    {"sim.getObjectChild",_simGetObjectChild,                    "number childObjectHandle=sim.getObjectChild(number objectHandle,number index)",true},
    {"sim.getObjectParent",_simGetObjectParent,                  "number parentObjectHandle=sim.getObjectParent(number objectHandle)",true},
    {"sim.setObjectParent",_simSetObjectParent,                  "sim.setObjectParent(number objectHandle,number parentObjectHandle,boolean keepInPlace)",true},
    {"sim.getObjectType",_simGetObjectType,                      "number objectType=sim.getObjectType(number objectHandle)",true},
    {"sim.getJointType",_simGetJointType,                        "number jointType=sim.getJointType(number objectHandle)",true},
    {"sim.setBoolParameter",_simSetBoolParameter,                "sim.setBoolParameter(number parameter,boolean boolState)",true},
    {"sim.getBoolParameter",_simGetBoolParameter,                "boolean boolState=sim.getBoolParameter(number parameter)",true},
    {"sim.setInt32Parameter",_simSetInt32Parameter,              "sim.setInt32Parameter(number parameter,number intState)",true},
    {"sim.getInt32Parameter",_simGetInt32Parameter,              "number intState=sim.getInt32Parameter(number parameter)",true},
    {"sim.setFloatParameter",_simSetFloatParameter,              "sim.setFloatParameter(number parameter,number floatState)",true},
    {"sim.getFloatParameter",_simGetFloatParameter,              "number floatState=sim.getFloatParameter(number parameter)",true},
    {"sim.setStringParameter",_simSetStringParameter,            "sim.setStringParameter(number parameter,string stringState)",true},
    {"sim.getStringParameter",_simGetStringParameter,            "string stringState=sim.getStringParameter(number parameter)",true},
    {"sim.setArrayParameter",_simSetArrayParameter,              "sim.setArrayParameter(number parameter,table arrayOfValues)",true},
    {"sim.getArrayParameter",_simGetArrayParameter,              "table arrayOfValues=sim.getArrayParameter(number parameter)",true},
    {"sim.setObjectName",_simSetObjectName,                      "sim.setObjectName(number objectHandle,string objectName)",true},
    {"sim.setCollectionName",_simSetCollectionName,              "sim.setCollectionName(number collectionHandle,string collectionName)",true},
    {"sim.getJointInterval",_simGetJointInterval,                "boolean cyclic,table_2 interval=sim.getJointInterval(number objectHandle)",true},
    {"sim.setJointInterval",_simSetJointInterval,                "sim.setJointInterval(number objectHandle,boolean cyclic,table_2 interval)",true},
    {"sim.loadScene",_simLoadScene,                              "sim.loadScene(string filename)",true},
    {"sim.closeScene",_simCloseScene,                            "number result=sim.closeScene()",true},
    {"sim.saveScene",_simSaveScene,                              "sim.saveScene(string filename)",true},
    {"sim.loadModel",_simLoadModel,                              "number objectHandle=sim.loadModel(string filename)",true},
    {"sim.saveModel",_simSaveModel,                              "string buffer=sim.saveModel(number modelBaseHandle,string filename=nil)",true},
    {"sim.isObjectInSelection",_simIsObjectInSelection,          "number selectionState=sim.isObjectInSelection(number objectHandle)",true},
    {"sim.addObjectToSelection",_simAddObjectToSelection,        "sim.addObjectToSelection(number what,number objectHandle)\nsim.addObjectToSelection(table objectHandles)",true},
    {"sim.removeObjectFromSelection",_simRemoveObjectFromSelection,"sim.removeObjectFromSelection(number what,number objectHandle)\nnumber result=sim.removeObjectFromSelection(table objectHandles)",true},
    {"sim.getObjectSelection",_simGetObjectSelection,            "table selectedObjectHandles=sim.getObjectSelection()",true},
    {"sim.getRealTimeSimulation",_simGetRealTimeSimulation,      "number result=sim.getRealTimeSimulation()",true},
    {"sim.setNavigationMode",_simSetNavigationMode,              "sim.setNavigationMode(number navigationMode)",true},
    {"sim.getNavigationMode",_simGetNavigationMode,              "number navigationMode=sim.getNavigationMode()",true},
    {"sim.setPage",_simSetPage,                                  "sim.setPage(number pageIndex)",true},
    {"sim.getPage",_simGetPage,                                  "number pageIndex=sim.getPage()",true},
    {"sim.copyPasteObjects",_simCopyPasteObjects,                "table copiedObjectHandles=sim.copyPasteObjects(table objectHandles,number options)",true},
    {"sim.scaleObjects",_simScaleObjects,                        "sim.scaleObjects(table objectHandles,number scalingFactor,boolean scalePositionsToo)",true},
    {"sim.getObjectUniqueIdentifier",_simGetObjectUniqueIdentifier,"number uniqueIdentifier=sim.getObjectUniqueIdentifier(number objectHandle)\ntable uniqueIdentifiers=sim.getObjectUniqueIdentifier(sim.handle_all)",true},
    {"sim.setThreadAutomaticSwitch",_simSetThreadAutomaticSwitch,"number autoSwitchForbidLevel=sim.setThreadAutomaticSwitch(boolean automaticSwitch/number forbidLevel)",true},
    {"sim.getThreadAutomaticSwitch",_simGetThreadAutomaticSwitch,"boolean result=sim.getThreadAutomaticSwitch()",true},
    {"sim.getThreadSwitchAllowed",_simGetThreadSwitchAllowed,    "boolean allowed=sim.getThreadSwitchAllowed()",true},
    {"sim.setThreadSwitchAllowed",_simSetThreadSwitchAllowed,    "number forbidLevel=sim.setThreadSwitchAllowed(boolean allowed/number forbidLevel)",true},
    {"sim.setThreadSwitchTiming",_simSetThreadSwitchTiming,      "sim.setThreadSwitchTiming(number deltaTimeInMilliseconds)",true},
    {"sim._switchThread",_simSwitchThread,                       "sim._switchThread()",false},
    {"sim.createCollection",_simCreateCollection,                "number collectionHandle=sim.createCollection(string collectionName,number options)",true},
    {"sim.addObjectToCollection",_simAddObjectToCollection,      "sim.addObjectToCollection(number collectionHandle,number objectHandle,number what,number options)",true},
    {"sim.saveImage",_simSaveImage,                              "string buffer=sim.saveImage(string image,table_2 resolution,number options,string filename,number quality)",true},
    {"sim.loadImage",_simLoadImage,                              "string image,table_2 resolution=sim.loadImage(number options,string filename)",true},
    {"sim.getScaledImage",_simGetScaledImage,                    "string imageOut,table_2 effectiveResolutionOut=sim.getScaledImage(string imageIn,table_2 resolutionIn,\ntable_2 desiredResolutionOut,number options)",true},
    {"sim.transformImage",_simTransformImage,                    "sim.transformImage(string image,table_2 resolution,number options)",true},
    {"sim.getQHull",_simGetQHull,                                "table verticesOut,table indicesOut=sim.getQHull(table verticesIn)",true},
    {"sim.getDecimatedMesh",_simGetDecimatedMesh,                "table verticesOut,table indicesOut=sim.getDecimatedMesh(table verticesIn,table indicesIn,number decimationPercentage)",true},
    {"sim.packInt32Table",_simPackInt32Table,                    "string data=sim.packInt32Table(table int32Numbers,number startInt32Index=0,number int32Count=0)",true},
    {"sim.packUInt32Table",_simPackUInt32Table,                  "string data=sim.packUInt32Table(table uint32Numbers,number startUInt32Index=0,number uint32Count=0)",true},
    {"sim.packFloatTable",_simPackFloatTable,                    "string data=sim.packFloatTable(table floatNumbers,number startFloatIndex=0,number floatCount=0)",true},
    {"sim.packDoubleTable",_simPackDoubleTable,                  "string data=sim.packDoubleTable(table doubleNumbers,number startDoubleIndex=0,number doubleCount=0)",true},
    {"sim.packUInt8Table",_simPackUInt8Table,                    "string data=sim.packUInt8Table(table uint8Numbers,number startUint8Index=0,number uint8count=0)",true},
    {"sim.packUInt16Table",_simPackUInt16Table,                  "string data=sim.packUInt16Table(table uint16Numbers,number startUint16Index=0,number uint16Count=0)",true},
    {"sim.unpackInt32Table",_simUnpackInt32Table,                "table int32Numbers=sim.unpackInt32Table(string data,number startInt32Index=0,number int32Count=0,number additionalByteOffset=0)",true},
    {"sim.unpackUInt32Table",_simUnpackUInt32Table,              "table uint32Numbers=sim.unpackUInt32Table(string data,number startUint32Index=0,number uint32Count=0,number additionalByteOffset=0)",true},
    {"sim.unpackFloatTable",_simUnpackFloatTable,                "table floatNumbers=sim.unpackFloatTable(string data,number startFloatIndex=0,number floatCount=0,number additionalByteOffset=0)",true},
    {"sim.unpackDoubleTable",_simUnpackDoubleTable,              "table doubleNumbers=sim.unpackDoubleTable(string data,number startDoubleIndex=0,number doubleCount=0,number additionalByteOffset=0)",true},
    {"sim.unpackUInt8Table",_simUnpackUInt8Table,                "table uint8Numbers=sim.unpackUInt8Table(string data,number startUint8Index=0,number uint8count=0)",true},
    {"sim.unpackUInt16Table",_simUnpackUInt16Table,              "table uint16Numbers=sim.unpackUInt16Table(string data,number startUint16Index=0,number uint16Count=0,number additionalByteOffset=0)",true},
    {"sim.packTable",_simPackTable,                              "string buffer=sim.packTable(table aTable)",true},
    {"sim.unpackTable",_simUnpackTable,                          "table aTable=sim.unpackTable(string buffer)",true},
    {"sim.transformBuffer",_simTransformBuffer,                  "string outBuffer=sim.transformBuffer(string inBuffer,number inFormat,number multiplier,number offset,number outFormat)",true},
    {"sim.combineRgbImages",_simCombineRgbImages,                "string outImg=sim.combineRgbImages(string img1,table_2 img1Res,string img2,table_2 img2Res,number operation)",true},
    {"sim.getVelocity",_simGetVelocity,                          "table_3 linearVelocity,table_3 angularVelocity=sim.getVelocity(number shapeHandle)",true},
    {"sim.getObjectVelocity",_simGetObjectVelocity,              "table_3 linearVelocity,table_3 angularVelocity=sim.getObjectVelocity(number objectHandle)",true},
    {"sim.addForceAndTorque",_simAddForceAndTorque,              "sim.addForceAndTorque(number shapeHandle,table_3 force=nil,table_3 torque=nil)",true},
    {"sim.addForce",_simAddForce,                                "sim.addForce(number shapeHandle,table_3 position,table_3 force)",true},
    {"sim.setExplicitHandling",_simSetExplicitHandling,          "sim.setExplicitHandling(number objectHandle,number explicitHandlingFlags)",true},
    {"sim.getExplicitHandling",_simGetExplicitHandling,          "number explicitHandlingFlags=sim.getExplicitHandling(number objectHandle)",true},
    {"sim.setGraphUserData",_simSetGraphUserData,                "number result=sim.setGraphUserData(number graphHandle,string dataStreamName,number data)",true},
    {"sim.addDrawingObject",_simAddDrawingObject,                "number drawingObjectHandle=sim.addDrawingObject(number objectType,number size,number duplicateTolerance,\nnumber parentObjectHandle,number maxItemCount,table_3 ambient_diffuse=nil,nil,table_3 specular=nil,\ntable_3 emission=nil)",true},
    {"sim.removeDrawingObject",_simRemoveDrawingObject,          "sim.removeDrawingObject(number drawingObjectHandle)",true},
    {"sim.addDrawingObjectItem",_simAddDrawingObjectItem,        "number result=sim.addDrawingObjectItem(number drawingObjectHandle,table itemData)",true},
    {"sim.addParticleObject",_simAddParticleObject,              "number particleObjectHandle=sim.addParticleObject(number objectType,number size,number density,table params,number lifeTime,\nnumber maxItemCount,table_3 ambient_diffuse=nil,nil,table_3 specular=nil,table_3 emission=nil)",true},
    {"sim.removeParticleObject",_simRemoveParticleObject,        "sim.removeParticleObject(number particleObjectHandle)",true},
    {"sim.addParticleObjectItem",_simAddParticleObjectItem,      "sim.addParticleObjectItem(number particleObjectHandle,table itemData)",true},
    {"sim.getObjectSizeFactor",_simGetObjectSizeFactor,          "number sizeFactor=sim.getObjectSizeFactor(number ObjectHandle)",true},
    {"sim.setIntegerSignal",_simSetIntegerSignal,                "sim.setIntegerSignal(string signalName,number signalValue)",true},
    {"sim.getIntegerSignal",_simGetIntegerSignal,                "number signalValue=sim.getIntegerSignal(string signalName)",true},
    {"sim.clearIntegerSignal",_simClearIntegerSignal,            "number clearCount=sim.clearIntegerSignal(string signalName)",true},
    {"sim.setFloatSignal",_simSetFloatSignal,                    "number result=sim.setFloatSignal(string signalName,number signalValue)",true},
    {"sim.getFloatSignal",_simGetFloatSignal,                    "number signalValue=sim.getFloatSignal(string signalName)",true},
    {"sim.clearFloatSignal",_simClearFloatSignal,                "number clearCount=sim.clearFloatSignal(string signalName)",true},
    {"sim.setDoubleSignal",_simSetDoubleSignal,                  "sim.setDoubleSignal(string signalName,number signalValue)",true},
    {"sim.getDoubleSignal",_simGetDoubleSignal,                  "number signalValue=sim.getDoubleSignal(string signalName)",true},
    {"sim.clearDoubleSignal",_simClearDoubleSignal,              "number clearCount=sim.clearDoubleSignal(string signalName)",true},
    {"sim.setStringSignal",_simSetStringSignal,                  "sim.setStringSignal(string signalName,string signalValue)",true},
    {"sim.getStringSignal",_simGetStringSignal,                  "string signalValue=sim.getStringSignal(string signalName)",true},
    {"sim.clearStringSignal",_simClearStringSignal,              "number clearCount=sim.clearStringSignal(string signalName)",true},
    {"sim.getSignalName",_simGetSignalName,                      "string signalName=sim.getSignalName(number signalIndex,number signalType)",true},
    {"sim.persistentDataWrite",_simPersistentDataWrite,          "sim.persistentDataWrite(string dataTag,string dataValue,number options=0)",true},
    {"sim.persistentDataRead",_simPersistentDataRead,            "string dataValue=sim.persistentDataRead(string dataTag)",true},
    {"sim.setObjectProperty",_simSetObjectProperty,              "sim.setObjectProperty(number objectHandle,number property)",true},
    {"sim.getObjectProperty",_simGetObjectProperty,              "number property=sim.getObjectProperty(number objectHandle)",true},
    {"sim.setObjectSpecialProperty",_simSetObjectSpecialProperty,"sim.setObjectSpecialProperty(number objectHandle,number property)",true},
    {"sim.getObjectSpecialProperty",_simGetObjectSpecialProperty,"number property=sim.getObjectSpecialProperty(number objectHandle)",true},
    {"sim.setModelProperty",_simSetModelProperty,                "sim.setModelProperty(number objectHandle,number property)",true},
    {"sim.getModelProperty",_simGetModelProperty,                "number property=sim.getModelProperty(number objectHandle)",true},
    {"sim.readForceSensor",_simReadForceSensor,                  "number result,table_3 forceVector,table_3 torqueVector=sim.readForceSensor(number objectHandle)",true},
    {"sim.breakForceSensor",_simBreakForceSensor,                "sim.breakForceSensor(number objectHandle)",true},
    {"sim.getLightParameters",_simGetLightParameters,            "number state,table_3 zero,table_3 diffusePart,table_3 specular=sim.getLightParameters(number lightHandle)",true},
    {"sim.setLightParameters",_simSetLightParameters,            "sim.setLightParameters(number lightHandle,number state,nil,table_3 diffusePart,table_3 specularPart)",true},
    {"sim.getLinkDummy",_simGetLinkDummy,                        "number linkDummyHandle=sim.getLinkDummy(number dummyHandle)",true},
    {"sim.setLinkDummy",_simSetLinkDummy,                        "sim.setLinkDummy(number dummyHandle,number linkDummyHandle)",true},
    {"sim.setShapeColor",_simSetShapeColor,                      "sim.setShapeColor(number shapeHandle,string colorName,number colorComponent,table_3 rgbData)",true},
    {"sim.getShapeColor",_simGetShapeColor,                      "number result,table_3 rgbData=sim.getShapeColor(number shapeHandle,string colorName,number colorComponent)",true},
    {"sim.resetDynamicObject",_simResetDynamicObject,            "sim.resetDynamicObject(number objectHandle)",true},
    {"sim.setJointMode",_simSetJointMode,                        "sim.setJointMode(number jointHandle,number jointMode,number options)",true},
    {"sim.getJointMode",_simGetJointMode,                        "number jointMode,number options=sim.getJointMode(number jointHandle)",true},
    {"sim._serialOpen",_simSerialOpen,                           "",false}, // partially implemented in sim.lua
    {"sim._serialClose",_simSerialClose,                         "",false}, // partially implemented in sim.lua
    {"sim.serialSend",_simSerialSend,                            "number charsSent=sim.serialSend(number portHandle,string data)",true},
    {"sim._serialRead",_simSerialRead,                           "",false}, // partially implemented in sim.lua
    {"sim.serialCheck",_simSerialCheck,                          "number byteCount=sim.serialCheck(number portHandle)",true},
    {"sim.getContactInfo",_simGetContactInfo,                    "table_2 collidingObjects,table_3 collisionPoint,table_3 reactionForce,table_3 normalVector=sim.getContactInfo(number dynamicPass,\nnumber objectHandle,number index)",true},
    {"sim.auxiliaryConsoleOpen",_simAuxiliaryConsoleOpen,        "number consoleHandle=sim.auxiliaryConsoleOpen(string title,number maxLines,number mode,table_2 position=nil,table_2 size=nil,\ntable_3 textColor=nil,table_3 backgroundColor=nil)",true},
    {"sim.auxiliaryConsoleClose",_simAuxiliaryConsoleClose,      "number result=sim.auxiliaryConsoleClose(number consoleHandle)",true},
    {"sim.auxiliaryConsolePrint",_simAuxiliaryConsolePrint,      "number result=sim.auxiliaryConsolePrint(number consoleHandle,string text)",true},
    {"sim.auxiliaryConsoleShow",_simAuxiliaryConsoleShow,        "number result=sim.auxiliaryConsoleShow(number consoleHandle,boolean showState)",true},
    {"sim.importShape",_simImportShape,                          "number shapeHandle=sim.importShape(number fileformat,string pathAndFilename,number options,number identicalVerticeTolerance\n,number scalingFactor)",true},
    {"sim.importMesh",_simImportMesh,                            "table_of_table vertices,table_of_table indices=sim.importMesh(number fileformat,string pathAndFilename,\nnumber options,number identicalVerticeTolerance,number scalingFactor)",true},
    {"sim.exportMesh",_simExportMesh,                            "sim.exportMesh(number fileformat,string pathAndFilename,number options,number scalingFactor,\ntable_of_table vertices,table_of_table indices)",true},
    {"sim.createMeshShape",_simCreateMeshShape,                  "number objectHandle=sim.createMeshShape(number options,number shadingAngle,table vertices,table indices)",true},
    {"sim.getShapeMesh",_simGetShapeMesh,                        "table vertices,table indices,table normals=sim.getShapeMesh(number shapeHandle)",true},
    {"sim.createPureShape",_simCreatePureShape,                  "number objectHandle=sim.createPureShape(number primitiveType,number options,table_3 sizes,number mass,table_2 precision=nil)",true},
    {"sim.createHeightfieldShape",_simCreateHeightfieldShape,    "number objectHandle=sim.createHeightfieldShape(number options,number shadingAngle,number xPointCount,\nnumber yPointCount,number xSize,table heights)",true},
    {"sim.addBanner",_simAddBanner,                              "number bannerID=sim.addBanner(string label,number size,number options,table_6 positionAndEulerAngles=nil,\nnumber parentObjectHandle=nil,table_12 labelColors=nil,table_12 backgroundColors=nil)",true},
    {"sim.removeBanner",_simRemoveBanner,                        "sim.removeBanner(number bannerID)",true},
    {"sim.createJoint",_simCreateJoint,                          "number jointHandle=sim.createJoint(number jointType,number jointMode,number options,table_2 sizes=nil,\ntable_12 colorA=nil,table_12 colorB=nil)",true},
    {"sim.createDummy",_simCreateDummy,                          "number dummyHandle=sim.createDummy(number size,table_12 color=nil)",true},
    {"sim.createProximitySensor",_simCreateProximitySensor,      "number sensorHandle=sim.createProximitySensor(number sensorType,number subType,number options,table_8 intParams,\ntable_15 floatParams,table_48 color=nil)",true},
    {"sim.createForceSensor",_simCreateForceSensor,              "number sensorHandle=sim.createForceSensor(number options,table_5 intParams,table_5 floatParams,table_24 color=nil)",true},
    {"sim.createVisionSensor",_simCreateVisionSensor,            "number sensorHandle=sim.createVisionSensor(number options,table_4 intParams,table_11 floatParams,table_48 color=nil)",true},
    {"sim.floatingViewAdd",_simFloatingViewAdd,                  "number floatingViewHandle=sim.floatingViewAdd(number posX,number posY,number sizeX,number sizeY,number options)",true},
    {"sim.floatingViewRemove",_simFloatingViewRemove,            "number result=sim.floatingViewRemove(number floatingViewHandle)",true},
    {"sim.adjustView",_simAdjustView,                            "number result=sim.adjustView(number viewHandleOrIndex,number associatedViewableObjectHandle,number options,string viewLabel=nil)",true},
    {"sim.cameraFitToView",_simCameraFitToView,                  "number result=sim.cameraFitToView(number viewHandleOrIndex,table objectHandles=nil,simInt options=0,simFloat scaling=1)",true},
    {"sim.announceSceneContentChange",_simAnnounceSceneContentChange,"number result=sim.announceSceneContentChange()",true},
    {"sim.getObjectInt32Parameter",_simGetObjectInt32Parameter,  "number result,number parameter=sim.getObjectInt32Parameter(number objectHandle,number parameterID)",true},
    {"sim.setObjectInt32Parameter",_simSetObjectInt32Parameter,  "sim.setObjectInt32Parameter(number objectHandle,number parameterID,number parameter)",true},
    {"sim.getObjectFloatParameter",_simGetObjectFloatParameter,  "number result,number parameter=sim.getObjectFloatParameter(number objectHandle,number parameterID)",true},
    {"sim.setObjectFloatParameter",_simSetObjectFloatParameter,  "sim.setObjectFloatParameter(number objectHandle,number parameterID,number parameter)",true},
    {"sim.getObjectStringParameter",_simGetObjectStringParameter,"string parameter=sim.getObjectStringParameter(number objectHandle,number parameterID)",true},
    {"sim.setObjectStringParameter",_simSetObjectStringParameter,"sim.setObjectStringParameter(number objectHandle,number parameterID,string parameter)",true},
    {"sim.getRotationAxis",_simGetRotationAxis,                  "table_3 axis,number angle=sim.getRotationAxis(table_12 matrixStart,table_12 matrixGoal)",true},
    {"sim.rotateAroundAxis",_simRotateAroundAxis,                "table_12 matrixOut=sim.rotateAroundAxis(table_12 matrixIn,table_3 axis,table_3 axisPos,number angle)",true},
    {"sim.launchExecutable",_simLaunchExecutable,                "sim.launchExecutable(string filename,string parameters='',number showStatus=1)",true},
    {"sim.getJointForce",_simGetJointForce,                      "number forceOrTorque=sim.getJointForce(number jointHandle)",true},
    {"sim.getJointMaxForce",_simGetJointMaxForce,                "number forceOrTorque=sim.getJointMaxForce(number jointHandle)",true},
    {"sim.isHandleValid",_simIsHandleValid,                      "number result=sim.isHandleValid(number generalObjectHandle,number generalObjectType=-1)",true},
    {"sim.getObjectQuaternion",_simGetObjectQuaternion,          "table_4 quaternion=sim.getObjectQuaternion(number objectHandle,number relativeToObjectHandle)",true},
    {"sim.setObjectQuaternion",_simSetObjectQuaternion,          "sim.setObjectQuaternion(number objectHandle,number relativeToObjectHandle,table_4 quaternion)",true},
    {"sim.groupShapes",_simGroupShapes,                          "number shapeHandle=sim.groupShapes(table shapeHandles)",true},
    {"sim.ungroupShape",_simUngroupShape,                        "table simpleShapeHandles=sim.ungroupShape(number shapeHandle)",true},
    {"sim.convexDecompose",_simConvexDecompose,                  "number shapeHandle=sim.convexDecompose(number shapeHandle,number options,table_4 intParams,table_3 floatParams)",true},
    {"sim.addGhost",_simAddGhost,                                "number ghostId=sim.addGhost(number ghostGroup,number objectHandle,number options,number startTime,number endTime,table_12 color=nil)",true},
    {"sim.modifyGhost",_simModifyGhost,                          "number result=sim.modifyGhost(number ghostGroup,number ghostId,number operation,number floatValue,number options=nil,\nnumber optionsMask=nil,table colorOrTransformation=nil)",true},
    {"sim.quitSimulator",_simQuitSimulator,                      "sim.quitSimulator()",true},
    {"sim.getThreadId",_simGetThreadId,                          "number threadId=sim.getThreadId()",true},
    {"sim.setShapeMaterial",_simSetShapeMaterial,                "sim.setShapeMaterial(number shapeHandle,number materialIdOrShapeHandle)",true},
    {"sim.getTextureId",_simGetTextureId,                        "number textureId,table_2 resolution=sim.getTextureId(string textureName)",true},
    {"sim.readTexture",_simReadTexture,                          "string textureData=sim.readTexture(number textureId,number options,number posX=0,number posY=0,number sizeX=0,number sizeY=0)",true},
    {"sim.writeTexture",_simWriteTexture,                        "sim.writeTexture(number textureId,number options,string textureData,number posX=0,number posY=0,number sizeX=0,\nnumber sizeY=0,number interpol=0)",true},
    {"sim.createTexture",_simCreateTexture,                      "number shapeHandle,number textureId,table_2 resolution=sim.createTexture(string fileName,number options,table_2 planeSizes=nil,\ntable_2 scalingUV=nil,table_2 xy_g=nil,number fixedResolution=0,table_2 resolution=nil)",true},
    {"sim.writeCustomDataBlock",_simWriteCustomDataBlock,        "sim.writeCustomDataBlock(number objectHandle,string tagName,string data)",true},
    {"sim.readCustomDataBlock",_simReadCustomDataBlock,          "string data=sim.readCustomDataBlock(number objectHandle,string tagName)",true},
    {"sim.readCustomDataBlockTags",_simReadCustomDataBlockTags,  "table tags=sim.readCustomDataBlockTags(number objectHandle)",true},
    {"sim.addPointCloud",_simAddPointCloud,                      "number pointCloudHandle=sim.addPointCloud(number pageMask,number layerMask,number objectHandle,number options,number pointSize,\ntable pointCoordinates,table_12 defaultColors=nil,table pointColors=nil,table pointNormals=nil)",true},
    {"sim.modifyPointCloud",_simModifyPointCloud,                "number result=sim.modifyPointCloud(number pointCloudHandle,number operation)",true},
    {"sim.getShapeGeomInfo",_simGetShapeGeomInfo,                "number result,number pureType,table_4 dimensions=sim.getShapeGeomInfo(number shapeHandle)",true},
    {"sim.getObjectsInTree",_simGetObjectsInTree,                "table objects=sim.getObjectsInTree(number treeBaseHandle,number objectType=sim.handle_all,number options=0)",true},
    {"sim.setObjectSizeValues",_simSetObjectSizeValues,          "sim.setObjectSizeValues(number objectHandle,table_3 sizeValues)",true},
    {"sim.getObjectSizeValues",_simGetObjectSizeValues,          "table_3 sizeValues=sim.getObjectSizeValues(number objectHandle)",true},
    {"sim.scaleObject",_simScaleObject,                          "sim.scaleObject(number objectHandle,number xScale,number yScale,number zScale,number options=0)",true},
    {"sim.setShapeTexture",_simSetShapeTexture,                  "sim.setShapeTexture(number shapeHandle,number textureId,number mappingMode,number options,table_2 uvScaling,\ntable_3 position=nil,table_3 orientation=nil)",true},
    {"sim.getShapeTextureId",_simGetShapeTextureId,              "number textureId=sim.getShapeTextureId(number shapeHandle)",true},
    {"sim.getCollectionObjects",_simGetCollectionObjects,        "table objectHandles=sim.getCollectionObjects(number collectionHandle)",true},
    {"sim.handleCustomizationScripts",_simHandleCustomizationScripts,"number count=sim.handleCustomizationScripts(number callType)",true},
    {"sim.handleAddOnScripts",_simHandleAddOnScripts,            "number count=sim.handleAddOnScripts(number callType)",true},
    {"sim.handleSandboxScript",_simHandleSandboxScript,          "sim.handleSandboxScript(number callType)",true},
    {"sim.setScriptAttribute",_simSetScriptAttribute,            "sim.setScriptAttribute(number scriptHandle,number attributeID,number/boolean attribute)",true},
    {"sim.getScriptAttribute",_simGetScriptAttribute,            "number/boolean attribute=sim.getScriptAttribute(number scriptHandle,number attributeID)",true},
    {"sim.handleChildScripts",_simHandleChildScripts,            "number executedScriptCount=sim.handleChildScripts(number callType,...(objects to be passed))",true},
    {"sim.reorientShapeBoundingBox",_simReorientShapeBoundingBox,"number result=sim.reorientShapeBoundingBox(number shapeHandle,number relativeToHandle)",true},
    {"sim.handleVisionSensor",_simHandleVisionSensor,            "number detectionCount,table auxiliaryValuesPacket1,table auxiliaryValuesPacket2,etc.=sim.handleVisionSensor(number sensorHandle)",true},
    {"sim.readVisionSensor",_simReadVisionSensor,                "number result,table auxiliaryValues=sim.readVisionSensor(number sensorHandle)",true},
    {"sim.resetVisionSensor",_simResetVisionSensor,              "sim.resetVisionSensor(number sensorHandle)",true},
    {"sim.getVisionSensorResolution",_simGetVisionSensorResolution,"table_2 resolution=sim.getVisionSensorResolution(number sensorHandle)",true},
    {"sim.getVisionSensorImage",_simGetVisionSensorImage,        "table/string imageBuffer=sim.getVisionSensorImage(number sensorHandle,number posX=0,number posY=0,number sizeX=0,\nnumber sizeY=0,number returnType=0)",true},
    {"sim.setVisionSensorImage",_simSetVisionSensorImage,        "number result=sim.setVisionSensorImage(number sensorHandle,table imageBuffer)\nnumber result=sim.setVisionSensorImage(number sensorHandle,string imageBuffer)",true},
    {"sim.getVisionSensorCharImage",_simGetVisionSensorCharImage,"string imageBuffer,number resolutionX,number resolutionY=sim.getVisionSensorCharImage(number sensorHandle,number posX=0,\nnumber posY=0,number sizeX=0,number sizeY=0,number RgbaCutoff=0)",true},
    {"sim.setVisionSensorCharImage",_simSetVisionSensorCharImage,"number result=sim.setVisionSensorCharImage(number sensorHandle,string imageBuffer)",true},
    {"sim.getVisionSensorDepthBuffer",_simGetVisionSensorDepthBuffer,"table/string depthBuffer=sim.getVisionSensorDepthBuffer(number sensorHandle,number posX=0,number posY=0,\nnumber sizeX=0,number sizeY=0)",true},
    {"sim.checkVisionSensor",_simCheckVisionSensor,              "number result,table auxiliaryValuesPacket1,table auxiliaryValuesPacket2,etc.=sim.checkVisionSensor(number sensorHandle,\nnumber entityHandle)",true},
    {"sim.checkVisionSensorEx",_simCheckVisionSensorEx,          "table buffer=sim.checkVisionSensorEx(number sensorHandle,number entityHandle,boolean returnImage)",true},
    {"sim.rmlPos",_simRMLPos,                                    "number handle=sim.rmlPos(number dofs,number smallestTimeStep,number flags,table currentPosVelAccel,table maxVelAccelJerk,\ntable selection,table targetPosVel)",true},
    {"sim.rmlVel",_simRMLVel,                                    "number handle=sim.rmlVel(number dofs,number smallestTimeStep,number flags,table currentPosVelAccel,table maxAccelJerk,\ntable selection,table targetVel)",true},
    {"sim.rmlStep",_simRMLStep,                                  "number result,table newPosVelAccel,number synchronizationTime=sim.rmlStep(number handle,number timeStep)",true},
    {"sim.rmlRemove",_simRMLRemove,                              "sim.rmlRemove(number handle)",true},
    {"sim.buildMatrixQ",_simBuildMatrixQ,                        "table_12 matrix=sim.buildMatrixQ(table_3 position,table_4 quaternion)",true},
    {"sim.getQuaternionFromMatrix",_simGetQuaternionFromMatrix,  "table_4 quaternion=sim.getQuaternionFromMatrix(table_12 matrix)",true},
    {"sim.fileDialog",_simFileDialog,                            "string pathAndName=sim.fileDialog(number mode,string title,string startPath,string initName,string extName,string ext)",true},
    {"sim.msgBox",_simMsgBox,                                    "number returnValue=sim.msgBox(number dlgType,number buttons,string title,string message)",true},
    {"sim.loadModule",_simLoadModule,                            "number pluginHandle=sim.loadModule(string filenameAndPath,string pluginName)",true},
    {"sim.unloadModule",_simUnloadModule,                        "number result=sim.unloadModule(number pluginHandle)",true},
    {"sim.callScriptFunction",_simCallScriptFunction,            "...=sim.callScriptFunction(string functionNameAtScriptName,number scriptHandleOrType,...)",true},
    {"sim.getExtensionString",_simGetExtensionString,            "string theString=sim.getExtensionString(number objectHandle,number index,string key=nil)",true},
    {"sim.computeMassAndInertia",_simComputeMassAndInertia,      "number result=sim.computeMassAndInertia(number shapeHandle,number density)",true},
    {"sim.setScriptVariable",_simSetScriptVariable,              "sim.setScriptVariable(string variableNameAtScriptName,number scriptHandleOrType,variable)",true},
    {"sim.getEngineFloatParameter",_simGetEngineFloatParameter,  "number floatParam=sim.getEngineFloatParameter(number paramId,number objectHandle)",true},
    {"sim.getEngineInt32Parameter",_simGetEngineInt32Parameter,  "number int32Param=sim.getEngineInt32Parameter(number paramId,number objectHandle)",true},
    {"sim.getEngineBoolParameter",_simGetEngineBoolParameter,    "boolean boolParam=sim.getEngineBoolParameter(number paramId,number objectHandle)",true},
    {"sim.setEngineFloatParameter",_simSetEngineFloatParameter,  "sim.setEngineFloatParameter(number paramId,number objectHandle,number floatParam)",true},
    {"sim.setEngineInt32Parameter",_simSetEngineInt32Parameter,  "sim.setEngineInt32Parameter(number paramId,number objectHandle,number int32Param)",true},
    {"sim.setEngineBoolParameter",_simSetEngineBoolParameter,    "sim.setEngineBoolParameter(number paramId,number objectHandle,boolean boolParam)",true},
    {"sim.createOctree",_simCreateOctree,                        "number handle=sim.createOctree(number voxelSize,number options,number pointSize)",true},
    {"sim.createPointCloud",_simCreatePointCloud,                "number handle=sim.createPointCloud(number maxVoxelSize,number maxPtCntPerVoxel,number options,number pointSize)",true},
    {"sim.setPointCloudOptions",_simSetPointCloudOptions,        "sim.setPointCloudOptions(number pointCloudHandle,number maxVoxelSize,\nnumber maxPtCntPerVoxel,number options,number pointSize)",true},
    {"sim.getPointCloudOptions",_simGetPointCloudOptions,        "number maxVoxelSize,number maxPtCntPerVoxel,number options,number pointSize=\nsim.getPointCloudOptions(number pointCloudHandle)",true},
    {"sim.insertVoxelsIntoOctree",_simInsertVoxelsIntoOctree,    "number totalVoxelCnt=sim.insertVoxelsIntoOctree(number octreeHandle,number options,table points,table color=nil,table tag=nil)",true},
    {"sim.removeVoxelsFromOctree",_simRemoveVoxelsFromOctree,    "number totalVoxelCnt=sim.removeVoxelsFromOctree(number octreeHandle,number options,table points)",true},
    {"sim.insertPointsIntoPointCloud",_simInsertPointsIntoPointCloud,"number totalPointCnt=sim.insertPointsIntoPointCloud(number pointCloudHandle,\nnumber options,table points,table color=nil,number duplicateTolerance=nil)",true},
    {"sim.removePointsFromPointCloud",_simRemovePointsFromPointCloud,"number totalPointCnt=sim.removePointsFromPointCloud(number pointCloudHandle,\nnumber options,table points,number tolerance)",true},
    {"sim.intersectPointsWithPointCloud",_simIntersectPointsWithPointCloud,"number totalPointCnt=sim.intersectPointsWithPointCloud(number pointCloudHandle,\nnumber options,table points,number tolerance)",true},
    {"sim.getOctreeVoxels",_simGetOctreeVoxels,                  "table voxels=sim.getOctreeVoxels(number octreeHandle)",true},
    {"sim.getPointCloudPoints",_simGetPointCloudPoints,          "table points=sim.getPointCloudPoints(number pointCloudHandle)",true},
    {"sim.insertObjectIntoOctree",_simInsertObjectIntoOctree,    "number totalVoxelCnt=sim.insertObjectIntoOctree(number octreeHandle,number objectHandle,number options,table color=nil,number tag=0)",true},
    {"sim.subtractObjectFromOctree",_simSubtractObjectFromOctree,    "number totalVoxelCnt=sim.subtractObjectFromOctree(number octreeHandle,number objectHandle,number options)",true},
    {"sim.insertObjectIntoPointCloud",_simInsertObjectIntoPointCloud,"number totalPointCnt=sim.insertObjectIntoPointCloud(number pointCloudHandle,\nnumber objectHandle,number options,number gridSize,table color=nil,number duplicateTolerance=nil)",true},
    {"sim.subtractObjectFromPointCloud",_simSubtractObjectFromPointCloud,    "number totalPointCnt=sim.subtractObjectFromPointCloud(number pointCloudHandle,number objectHandle,number options,number tolerance)",true},
    {"sim.checkOctreePointOccupancy",_simCheckOctreePointOccupancy,"number result,number tag,number locationLow,number locationHigh=sim.checkOctreePointOccupancy(number octreeHandle,number options,table points)",true},
    {"sim.handleSimulationStart",_simHandleSimulationStart,      "sim.handleSimulationStart()",true},
    {"sim.handleSensingStart",_simHandleSensingStart,            "sim.handleSensingStart()",true},
    {"sim.auxFunc",_simAuxFunc,                                  "... =sim.auxFunc(...)",true},
    {"sim.setReferencedHandles",_simSetReferencedHandles,        "sim.setReferencedHandles(number objectHandle,table referencedHandles)",true},
    {"sim.getReferencedHandles",_simGetReferencedHandles,        "table referencedHandles=sim.getReferencedHandles(number objectHandle)",true},
    {"sim.getGraphCurve",_simGetGraphCurve,                      "string label,number curveType,table curveColor,table xData,table yData,table zData,table minMax=\nsim.getGraphCurve(number graphHandle,number graphType,number curveIndex)",true},
    {"sim.getGraphInfo",_simGetGraphInfo,                        "number bitCoded,table_3 bgColor,table_3 fgColor=sim.getGraphInfo(number graphHandle)",true},
    {"sim.getShapeViz",_simGetShapeViz,                          "map data=sim.getShapeViz(number shapeHandle,number itemIndex)",true},
    {"sim.executeScriptString",_simExecuteScriptString,          "number result,executionResult=sim.executeScriptString(string stringAtScriptName,number scriptHandleOrType)",true},
    {"sim.getApiFunc",_simGetApiFunc,                            "table funcsAndVars=sim.getApiFunc(number scriptHandleOrType,string apiWord)",true},
    {"sim.getApiInfo",_simGetApiInfo,                            "string info=sim.getApiInfo(number scriptHandleOrType,string apiWord)",true},
    {"sim.getModuleInfo",_simGetModuleInfo,                      "string/number info=sim.getModuleInfo(string moduleName,number infoType)",true},
    {"sim.setModuleInfo",_simSetModuleInfo,                      "sim.setModuleInfo(string moduleName,number infoType,string/number info)",true},
    {"sim.registerScriptFunction",_simRegisterScriptFunction,    "number result=sim.registerScriptFunction(string funcNameAtPluginName,string callTips)",true},
    {"sim.registerScriptVariable",_simRegisterScriptVariable,    "number result=sim.registerScriptVariable(string varNameAtPluginName)",true},
    {"sim.isDeprecated",_simIsDeprecated,                        "number result=sim.isDeprecated(string funcOrConst)",true},
    {"sim.getPersistentDataTags",_simGetPersistentDataTags,      "table tags=sim.getPersistentDataTags()",true},
    {"sim.getRandom",_simGetRandom,                              "number randomNumber=sim.getRandom(number seed=nil)",true},
    {"sim.textEditorOpen",_simTextEditorOpen,                    "number handle=sim.textEditorOpen(string initText,string properties)",true},
    {"sim.textEditorClose",_simTextEditorClose,                  "string text,table_2 pos,table_2 size=sim.textEditorClose(number handle)",true},
    {"sim.textEditorShow",_simTextEditorShow,                    "sim.textEditorShow(number handle,boolean showState)",true},
    {"sim.textEditorGetInfo",_simTextEditorGetInfo,              "string text,table_2 pos,table_2 size,boolean visible=sim.textEditorGetInfo(number handle)",true},
    {"sim.setJointDependency",_simSetJointDependency,            "sim.setJointDependency(number jointHandle,number masterJointHandle,number offset,number multCoeff)",true},
    {"sim.getJointDependency",_simGetJointDependency,            "number masterJointHandle,number offset,number multCoeff=sim.getJointDependency(number jointHandle)",true},
    {"sim.getStackTraceback",_simGetStackTraceback,              "string stacktraceback=sim.getStackTraceback([number scriptHandle])",true},
    {"sim.setStringNamedParam",_simSetStringNamedParam,          "number result=sim.setStringNamedParam(string paramName,string stringParam)",true},
    {"sim.getStringNamedParam",_simGetStringNamedParam,          "string stringParam=sim.getStringNamedParam(string paramName)",true},
    {"sim.addLog",_simAddLog,                                    "sim.addLog(number verbosityLevel,string logMessage)",true},
    {"sim.getShapeMass",_simGetShapeMass,                        "number mass=sim.getShapeMassAndInertia(number shapeHandle)",true},
    {"sim.setShapeMass",_simSetShapeMass,                        "sim.setShapeMass(number shapeHandle,number mass)",true},
    {"sim.getShapeInertia",_simGetShapeInertia,                  "table_9 inertiaMatrix,table_12 transformationMatrix=sim.getShapeInertia(number shapeHandle)",true},
    {"sim.setShapeInertia",_simSetShapeInertia,                  "sim.setShapeInertia(number shapeHandle,table_9 inertiaMatrix,table_12 transformationMatrix)",true},
    {"sim.isDynamicallyEnabled",_simIsDynamicallyEnabled,        "boolean enabled=sim.isDynamicallyEnabled(number objectHandle)",true},

    {"sim.test",_simTest,                                        "test function - shouldn't be used",true},



    // deprecated
    {"sim.addStatusbarMessage",_simAddStatusbarMessage,         "Deprecated. Use 'sim.addLog' instead",false},
    {"sim.getNameSuffix",_simGetNameSuffix,                     "Deprecated",false},
    {"sim.setNameSuffix",_simSetNameSuffix,                     "Deprecated",false},
    {"sim.resetMill",_simResetMill,                             "Deprecated. Has no effect",false},
    {"sim.resetMilling",_simResetMilling,                       "Deprecated. Has no effect",false},
    {"sim.handleMill",_simHandleMill,                           "Deprecated. Has no effect",false},
    {"sim.openTextEditor",_simOpenTextEditor,                    "Deprecated. Use 'sim.textEditorOpen' instead",false},
    {"sim.closeTextEditor",_simCloseTextEditor,                  "Deprecated. Use 'sim.textEditorClose' instead",false},
    {"simHandlePath",_simHandlePath,                                "Deprecated",false},
    {"simHandleJoint",_simHandleJoint,                              "Deprecated",false},
    {"sim.getScriptSimulationParameter",_simGetScriptSimulationParameter,"Deprecated. Use 'sim.getUserParameter' instead",false},
    {"sim.setScriptSimulationParameter",_simSetScriptSimulationParameter,"Deprecated. Use 'sim.setUserParameter' instead",false},
    {"sim.setJointForce",_simSetJointMaxForce,                   "Deprecated. Use 'sim.setJointMaxForce' instead",false},
    {"sim.handleMechanism",_simHandleMechanism,                  "Deprecated. Has no effect.",false},
    {"sim.setPathTargetNominalVelocity",_simSetPathTargetNominalVelocity,"Deprecated",false},
    {"sim.getShapeMassAndInertia",_simGetShapeMassAndInertia,    "Deprecated. Use 'sim.getShapeMass' and/or 'sim.getShapeInertia' instead",false},
    {"sim.setShapeMassAndInertia",_simSetShapeMassAndInertia,    "Deprecated. Use 'sim.setShapeMass' and/or 'sim.setShapeInertia' instead",false},
    {"sim.checkIkGroup",_simCheckIkGroup,                        "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.createIkGroup",_simCreateIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.removeIkGroup",_simRemoveIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.createIkElement",_simCreateIkElement,                  "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.exportIk",_simExportIk,                                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.computeJacobian",_simComputeJacobian,                  "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.getConfigForTipPose",_simGetConfigForTipPose,          "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.generateIkPath",_simGenerateIkPath,                    "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.getIkGroupHandle",_simGetIkGroupHandle,                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.getIkGroupMatrix",_simGetIkGroupMatrix,                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.handleIkGroup",_simHandleIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.setIkGroupProperties",_simSetIkGroupProperties,        "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.setIkElementProperties",_simSetIkElementProperties,    "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"sim.setThreadIsFree",_simSetThreadIsFree,                  "Deprecated. Has no effect.",false},
    {"sim._tubeRead",_simTubeRead,                               "",false},
    {"sim.tubeOpen",_simTubeOpen,                                "Deprecated. Use signals or custom data blocks instead",false},
    {"sim.tubeClose",_simTubeClose,                              "Deprecated. Use signals or custom data blocks instead",false},
    {"sim.tubeWrite",_simTubeWrite,                              "Deprecated. Use signals or custom data blocks instead",false},
    {"sim.tubeStatus",_simTubeStatus,                            "Deprecated. Use signals or custom data blocks instead",false},
    {"sim.sendData",_simSendData,                                "Deprecated. Use signals or custom data blocks instead",false},
    {"sim.receiveData",_simReceiveData,                          "Deprecated. Use signals or custom data blocks instead",false},

    {"sim.moveToPosition",_simMoveToPosition,                    "Deprecated. Use 'sim.moveToPose' instead",false},
    {"sim.moveToObject",_simMoveToObject,                        "Deprecated. Use 'sim.moveToPose' instead",false},
    {"sim.followPath",_simFollowPath,                            "Deprecated. Use 'sim.rmlPos' and 'sim.rmlStep' instead",false},
    {"sim.moveToJointPositions",_simMoveToJointPositions,        "Deprecated. Use 'sim.moveToConfig' instead",false},
    {"sim.getPathPosition",_simGetPathPosition,                  "Deprecated",false},
    {"sim.setPathPosition",_simSetPathPosition,                  "Deprecated",false},
    {"sim.getPathLength",_simGetPathLength,                      "Deprecated",false},
    {"sim.getDataOnPath",_simGetDataOnPath,                      "Deprecated",false},
    {"sim.getPositionOnPath",_simGetPositionOnPath,              "Deprecated",false},
    {"sim.getOrientationOnPath",_simGetOrientationOnPath,        "Deprecated",false},
    {"sim.getClosestPositionOnPath",_simGetClosestPositionOnPath,"Deprecated",false},
    {"sim.createPath",_simCreatePath,                            "Deprecated",false},
    {"sim.insertPathCtrlPoints",_simInsertPathCtrlPoints,        "Deprecated",false},
    {"sim.cutPathCtrlPoints",_simCutPathCtrlPoints,              "Deprecated",false},
    {"sim.getScriptExecutionCount",_simGetScriptExecutionCount,  "Deprecated",false},
    {"sim.isScriptExecutionThreaded",_simIsScriptExecutionThreaded,"Deprecated",false},
    {"sim.isScriptRunningInThread",_simIsScriptRunningInThread,  "Deprecated",false},
    {"sim.setThreadResumeLocation",_simSetThreadResumeLocation,  "Deprecated",false},
    {"sim.resumeThreads",_simResumeThreads,                      "Deprecated",false},
    {"sim.launchThreadedChildScripts",_simLaunchThreadedChildScripts,"Deprecated",false},
    {"sim.getUserParameter",_simGetUserParameter,                "Deprecated. Use 'sim.readCustomDataBlock' instead",false},
    {"sim.setUserParameter",_simSetUserParameter,                "Deprecated. Use 'sim.writeCustomDataBlock' instead",false},
    {"sim.setVisionSensorFilter",_simSetVisionSensorFilter,      "Deprecated. Use vision callback functions instead",false},
    {"sim.getVisionSensorFilter",_simGetVisionSensorFilter,      "Deprecated. Use vision callback functions instead",false},
    // {"sim.rmlMoveToPosition",_simRMLMoveToPosition,              "Deprecated. Use 'sim.moveToPose' instead",false},
    //{"sim.rmlMoveToJointPositions",_simRMLMoveToJointPositions,  "Deprecated. Use 'sim.moveToConfig' instead",false},
    //{"sim.wait",_simWait,                                        "number deltaTimeLeft=sim.wait(number deltaTime,boolean simulationTime=true)",true},
    //{"sim.waitForSignal",_simWaitForSignal,                      "number/string signalValue=sim.waitForSignal(string signalName)",true},

    {"",nullptr,"",false}
};


const SLuaCommands simLuaCommandsOldApi[]=
{ // Following for backward compatibility (has now a new notation (see 'simLuaCommands'))
    {"simGetScriptName",_simGetScriptName,                      "Use the newer 'sim.getScriptName' notation",false},
    {"simGetObjectAssociatedWithScript",_simGetObjectAssociatedWithScript,"Use the newer 'sim.getObjectAssociatedWithScript' notation",false},
    {"simGetScriptAssociatedWithObject",_simGetScriptAssociatedWithObject,"Use the newer 'sim.getScriptAssociatedWithObject' notation",false},
    {"simGetCustomizationScriptAssociatedWithObject",_simGetCustomizationScriptAssociatedWithObject,"Use the newer 'sim.getCustomizationScriptAssociatedWithObject' notation",false},
    {"simOpenModule",_simOpenModule,                            "Use the newer 'sim.openModule' notation",false},
    {"simCloseModule",_simCloseModule,                          "Use the newer 'sim.closeModule' notation",false},
    {"simHandleModule",_simHandleModule,                        "Use the newer 'sim.handleModule' notation",false},
    {"simBoolOr32",_simBoolOr32,                                "Use the newer 'sim.boolOr32' notation",false},
    {"simBoolAnd32",_simBoolAnd32,                              "Use the newer 'sim.boolAnd32' notation",false},
    {"simBoolXor32",_simBoolXor32,                              "Use the newer 'sim.boolXor32' notation",false},
    {"simHandleDynamics",_simHandleDynamics,                    "Use the newer 'sim.handleDynamics' notation",false},
    {"simHandleCollision",_simHandleCollision,                  "Use the newer 'sim.handleCollision' notation",false},
    {"simReadCollision",_simReadCollision,                      "Use the newer 'sim.readCollision' notation",false},
    {"simHandleDistance",_simHandleDistance,                    "Use the newer 'sim.handleDistance' notation",false},
    {"simReadDistance",_simReadDistance,                        "Use the newer 'sim.readDistance' notation",false},
    {"simHandleProximitySensor",_simHandleProximitySensor,      "Use the newer 'sim.handleProximitySensor' notation",false},
    {"simReadProximitySensor",_simReadProximitySensor,          "Use the newer 'sim.readProximitySensor' notation",false},
    {"simResetCollision",_simResetCollision,                    "Use the newer 'sim.resetCollision' notation",false},
    {"simResetDistance",_simResetDistance,                      "Use the newer 'sim.resetDistance' notation",false},
    {"simResetProximitySensor",_simResetProximitySensor,        "Use the newer 'sim.resetProximitySensor' notation",false},
    {"simCheckProximitySensor",_simCheckProximitySensor,        "Use the newer 'sim.checkProximitySensor' notation",false},
    {"simCheckProximitySensorEx",_simCheckProximitySensorEx,    "Use the newer 'sim.checkProximitySensorEx' notation",false},
    {"simCheckProximitySensorEx2",_simCheckProximitySensorEx2,  "Use the newer 'sim.checkProximitySensorEx2' notation",false},
    {"simGetObjectHandle",_simGetObjectHandle,                  "Use the newer 'sim.getObjectHandle' notation",false},
    {"simAddScript",_simAddScript,                              "Use the newer 'sim.addScript' notation",false},
    {"simAssociateScriptWithObject",_simAssociateScriptWithObject,"Use the newer 'sim.associateScriptWithObject' notation",false},
    {"simSetScriptText",_simSetScriptText,                      "Use the newer 'sim.setScriptText' notation",false},
    {"simGetScriptHandle",_simGetScriptHandle,                  "Use the newer 'sim.getScriptHandle' notation",false},
    {"simGetCollectionHandle",_simGetCollectionHandle,          "Use the newer 'sim.getCollectionHandle' notation",false},
    {"simRemoveCollection",_simRemoveCollection,                "Use the newer 'sim.removeCollection' notation",false},
    {"simEmptyCollection",_simEmptyCollection,                  "Use the newer 'sim.emptyCollection' notation",false},
    {"simGetObjectPosition",_simGetObjectPosition,              "Use the newer 'sim.getObjectPosition' notation",false},
    {"simGetObjectOrientation",_simGetObjectOrientation,        "Use the newer 'sim.getObjectOrientation' notation",false},
    {"simSetObjectPosition",_simSetObjectPosition,              "Use the newer 'sim.setObjectPosition' notation",false},
    {"simSetObjectOrientation",_simSetObjectOrientation,        "Use the newer 'sim.setObjectOrientation' notation",false},
    {"simGetJointPosition",_simGetJointPosition,                "Use the newer 'sim.getJointPosition' notation",false},
    {"simSetJointPosition",_simSetJointPosition,                "Use the newer 'sim.setJointPosition' notation",false},
    {"simSetJointTargetPosition",_simSetJointTargetPosition,    "Use the newer 'sim.setJointTargetPosition' notation",false},
    {"simGetJointTargetPosition",_simGetJointTargetPosition,    "Use the newer 'sim.getJointTargetPosition' notation",false},
    {"simSetJointForce",_simSetJointMaxForce,                   "Use the newer 'sim.setJointMaxForce' notation",false},
    {"simSetJointTargetVelocity",_simSetJointTargetVelocity,    "Use the newer 'sim.setJointTargetVelocity' notation",false},
    {"simGetJointTargetVelocity",_simGetJointTargetVelocity,    "Use the newer 'sim.getJointTargetVelocity' notation",false},
    {"simGetObjectName",_simGetObjectName,                      "Use the newer 'sim.getObjectName' notation",false},
    {"simGetCollectionName",_simGetCollectionName,              "Use the newer 'sim.getCollectionName' notation",false},
    {"simRemoveObject",_simRemoveObject,                        "Use the newer 'sim.removeObject' notation",false},
    {"simRemoveModel",_simRemoveModel,                          "Use the newer 'sim.removeModel' notation",false},
    {"simGetSimulationTime",_simGetSimulationTime,              "Use the newer 'sim.getSimulationTime' notation",false},
    {"simGetSimulationState",_simGetSimulationState,            "Use the newer 'sim.getSimulationState' notation",false},
    {"simGetSystemTime",_simGetSystemTime,                      "Use the newer 'sim.getSystemTime' notation",false},
    {"simGetSystemTimeInMs",_simGetSystemTimeInMs,              "Use the newer 'sim.getSystemTimeInMs' notation",false},
    {"simCheckCollision",_simCheckCollision,                    "Use the newer 'sim.checkCollision' notation",false},
    {"simCheckCollisionEx",_simCheckCollisionEx,                "Use the newer 'sim.checkCollisionEx' notation",false},
    {"simCheckDistance",_simCheckDistance,                      "Use the newer 'sim.checkDistance' notation",false},
    {"simGetObjectConfiguration",_simGetObjectConfiguration,    "Use the newer 'sim.getObjectConfiguration' notation",false},
    {"simSetObjectConfiguration",_simSetObjectConfiguration,    "Use the newer 'sim.setObjectConfiguration' notation",false},
    {"simGetConfigurationTree",_simGetConfigurationTree,        "Use the newer 'sim.getConfigurationTree' notation",false},
    {"simSetConfigurationTree",_simSetConfigurationTree,        "Use the newer 'sim.setConfigurationTree' notation",false},
    {"simGetSimulationTimeStep",_simGetSimulationTimeStep,      "Use the newer 'sim.getSimulationTimeStep' notation",false},
    {"simGetSimulatorMessage",_simGetSimulatorMessage,          "Use the newer 'sim.getSimulatorMessage' notation",false},
    {"simResetGraph",_simResetGraph,                            "Use the newer 'sim.resetGraph' notation",false},
    {"simHandleGraph",_simHandleGraph,                          "Use the newer 'sim.handleGraph' notation",false},
    {"simGetLastError",_simGetLastError,                        "Use the newer 'sim.getLastError' notation",false},
    {"simGetObjects",_simGetObjects,                            "Use the newer 'sim.getObjects' notation",false},
    {"simRefreshDialogs",_simRefreshDialogs,                    "Use the newer 'sim.refreshDialogs' notation",false},
    {"simGetModuleName",_simGetModuleName,                      "Use the newer 'sim.getModuleName' notation",false},
    {"simRemoveScript",_simRemoveScript,                        "Use the newer 'sim.removeScript' notation",false},
    {"simGetCollisionHandle",_simGetCollisionHandle,            "Use the newer 'sim.getCollisionHandle' notation",false},
    {"simGetDistanceHandle",_simGetDistanceHandle,              "Use the newer 'sim.getDistanceHandle' notation",false},
    {"simStopSimulation",_simStopSimulation,                    "Use the newer 'sim.stopSimulation' notation",false},
    {"simPauseSimulation",_simPauseSimulation,                  "Use the newer 'sim.pauseSimulation' notation",false},
    {"simStartSimulation",_simStartSimulation,                  "Use the newer 'sim.startSimulation' notation",false},
    {"simGetObjectMatrix",_simGetObjectMatrix,                  "Use the newer 'sim.getObjectMatrix' notation",false},
    {"simSetObjectMatrix",_simSetObjectMatrix,                  "Use the newer 'sim.setObjectMatrix' notation",false},
    {"simGetJointMatrix",_simGetJointMatrix,                    "Use the newer 'sim.getJointMatrix' notation",false},
    {"simSetSphericalJointMatrix",_simSetSphericalJointMatrix,  "Use the newer 'sim.setSphericalJointMatrix' notation",false},
    {"simBuildIdentityMatrix",_simBuildIdentityMatrix,          "Use the newer 'sim.buildIdentityMatrix' notation",false},
    {"simCopyMatrix",_simCopyMatrix,                            "Use the newer 'sim.copyMatrix' notation",false},
    {"simBuildMatrix",_simBuildMatrix,                          "Use the newer 'sim.buildMatrix' notation",false},
    {"simGetEulerAnglesFromMatrix",_simGetEulerAnglesFromMatrix,"Use the newer 'sim.getEulerAnglesFromMatrix' notation",false},
    {"simInvertMatrix",_simInvertMatrix,                        "Use the newer 'sim.invertMatrix' notation",false},
    {"simMultiplyMatrices",_simMultiplyMatrices,                "Use the newer 'sim.multiplyMatrices' notation",false},
    {"simInterpolateMatrices",_simInterpolateMatrices,          "Use the newer 'sim.interpolateMatrices' notation",false},
    {"simMultiplyVector",_simMultiplyVector,                    "Use the newer 'sim.multiplyVector' notation",false},
    {"simGetObjectChild",_simGetObjectChild,                    "Use the newer 'sim.getObjectChild' notation",false},
    {"simGetObjectParent",_simGetObjectParent,                  "Use the newer 'sim.getObjectParent' notation",false},
    {"simSetObjectParent",_simSetObjectParent,                  "Use the newer 'sim.setObjectParent' notation",false},
    {"simGetObjectType",_simGetObjectType,                      "Use the newer 'sim.getObjectType' notation",false},
    {"simGetJointType",_simGetJointType,                        "Use the newer 'sim.getJointType' notation",false},
    {"simSetBoolParameter",_simSetBoolParameter,                "Use the newer 'sim.setBoolParameter' notation",false},
    {"simGetBoolParameter",_simGetBoolParameter,                "Use the newer 'sim.getBoolParameter' notation",false},
    {"simSetInt32Parameter",_simSetInt32Parameter,              "Use the newer 'sim.setInt32Parameter' notation",false},
    {"simGetInt32Parameter",_simGetInt32Parameter,              "Use the newer 'sim.getInt32Parameter' notation",false},
    {"simSetFloatParameter",_simSetFloatParameter,              "Use the newer 'sim.setFloatParameter' notation",false},
    {"simGetFloatParameter",_simGetFloatParameter,              "Use the newer 'sim.getFloatParameter' notation",false},
    {"simSetStringParameter",_simSetStringParameter,            "Use the newer 'sim.setStringParameter' notation",false},
    {"simGetStringParameter",_simGetStringParameter,            "Use the newer 'sim.getStringParameter' notation",false},
    {"simSetArrayParameter",_simSetArrayParameter,              "Use the newer 'sim.setArrayParameter' notation",false},
    {"simGetArrayParameter",_simGetArrayParameter,              "Use the newer 'sim.getArrayParameter' notation",false},
    {"simSetObjectName",_simSetObjectName,                      "Use the newer 'sim.setObjectName' notation",false},
    {"simSetCollectionName",_simSetCollectionName,              "Use the newer 'sim.setCollectionName' notation",false},
    {"simGetJointInterval",_simGetJointInterval,                "Use the newer 'sim.getJointInterval' notation",false},
    {"simSetJointInterval",_simSetJointInterval,                "Use the newer 'sim.setJointInterval' notation",false},
    {"simLoadScene",_simLoadScene,                              "Use the newer 'sim.loadScene' notation",false},
    {"simSaveScene",_simSaveScene,                              "Use the newer 'sim.saveScene' notation",false},
    {"simLoadModel",_simLoadModel,                              "Use the newer 'sim.loadModel' notation",false},
    {"simSaveModel",_simSaveModel,                              "Use the newer 'sim.saveModel' notation",false},
    {"simIsObjectInSelection",_simIsObjectInSelection,          "Use the newer 'sim.isObjectInSelection' notation",false},
    {"simAddObjectToSelection",_simAddObjectToSelection,        "Use the newer 'sim.addObjectToSelection' notation",false},
    {"simRemoveObjectFromSelection",_simRemoveObjectFromSelection,"Use the newer 'sim.removeObjectFromSelection' notation",false},
    {"simGetObjectSelection",_simGetObjectSelection,            "Use the newer 'sim.getObjectSelection' notation",false},
    {"simGetRealTimeSimulation",_simGetRealTimeSimulation,      "Use the newer 'sim.getRealTimeSimulation' notation",false},
    {"simSetNavigationMode",_simSetNavigationMode,              "Use the newer 'sim.setNavigationMode' notation",false},
    {"simGetNavigationMode",_simGetNavigationMode,              "Use the newer 'sim.getNavigationMode' notation",false},
    {"simSetPage",_simSetPage,                                  "Use the newer 'sim.setPage' notation",false},
    {"simGetPage",_simGetPage,                                  "Use the newer 'sim.getPage' notation",false},
    {"simCopyPasteObjects",_simCopyPasteObjects,                "Use the newer 'sim.copyPasteObjects' notation",false},
    {"simScaleObjects",_simScaleObjects,                        "Use the newer 'sim.scaleObjects' notation",false},
    {"simGetObjectUniqueIdentifier",_simGetObjectUniqueIdentifier,"Use the newer 'sim.getObjectUniqueIdentifier' notation",false},
    {"simSetThreadAutomaticSwitch",_simSetThreadAutomaticSwitch,"Use the newer 'sim.setThreadAutomaticSwitch' notation",false},
    {"simGetThreadAutomaticSwitch",_simGetThreadAutomaticSwitch,"Use the newer 'sim.getThreadAutomaticSwitch' notation",false},
    {"simSetThreadSwitchTiming",_simSetThreadSwitchTiming,      "Use the newer 'sim.setThreadSwitchTiming' notation",false},
    {"simSwitchThread",_simSwitchThread,                        "Use the newer 'sim.switchThread' notation",false},
    {"simCreateCollection",_simCreateCollection,                "Use the newer 'sim.createCollection' notation",false},
    {"simAddObjectToCollection",_simAddObjectToCollection,      "Use the newer 'sim.addObjectToCollection' notation",false},
    {"simSaveImage",_simSaveImage,                              "Use the newer 'sim.saveImage' notation",false},
    {"simLoadImage",_simLoadImage,                              "Use the newer 'sim.loadImage' notation",false},
    {"simGetScaledImage",_simGetScaledImage,                    "Use the newer 'sim.getScaledImage' notation",false},
    {"simTransformImage",_simTransformImage,                    "Use the newer 'sim.transformImage' notation",false},
    {"simGetQHull",_simGetQHull,                                "Use the newer 'sim.getQHull' notation",false},
    {"simGetDecimatedMesh",_simGetDecimatedMesh,                "Use the newer 'sim.getDecimatedMesh' notation",false},
    {"simPackInt32Table",_simPackInt32Table,                    "Use the newer 'sim.packInt32Table' notation",false},
    {"simPackUInt32Table",_simPackUInt32Table,                  "Use the newer 'sim.packUInt32Table' notation",false},
    {"simPackFloatTable",_simPackFloatTable,                    "Use the newer 'sim.packFloatTable' notation",false},
    {"simPackDoubleTable",_simPackDoubleTable,                  "Use the newer 'sim.packDoubleTable' notation",false},
    {"simPackUInt8Table",_simPackUInt8Table,                    "Use the newer 'sim.packUInt8Table' notation",false},
    {"simPackUInt16Table",_simPackUInt16Table,                  "Use the newer 'sim.packUInt16Table' notation",false},
    {"simUnpackInt32Table",_simUnpackInt32Table,                "Use the newer 'sim.unpackInt32Table' notation",false},
    {"simUnpackUInt32Table",_simUnpackUInt32Table,              "Use the newer 'sim.unpackUInt32Table' notation",false},
    {"simUnpackFloatTable",_simUnpackFloatTable,                "Use the newer 'sim.unpackFloatTable' notation",false},
    {"simUnpackDoubleTable",_simUnpackDoubleTable,              "Use the newer 'sim.unpackDoubleTable' notation",false},
    {"simUnpackUInt8Table",_simUnpackUInt8Table,                "Use the newer 'sim.unpackUInt8Table' notation",false},
    {"simUnpackUInt16Table",_simUnpackUInt16Table,              "Use the newer 'sim.unpackUInt16Table' notation",false},
    {"simPackTable",_simPackTable,                              "Use the newer 'sim.packTable' notation",false},
    {"simUnpackTable",_simUnpackTable,                          "Use the newer 'sim.unpackTable' notation",false},
    {"simTransformBuffer",_simTransformBuffer,                  "Use the newer 'sim.transformBuffer' notation",false},
    {"simCombineRgbImages",_simCombineRgbImages,                "Use the newer 'sim.combineRgbImages' notation",false},
    {"simGetVelocity",_simGetVelocity,                          "Use the newer 'sim.getVelocity' notation",false},
    {"simGetObjectVelocity",_simGetObjectVelocity,              "Use the newer 'sim.getObjectVelocity' notation",false},
    {"simAddForceAndTorque",_simAddForceAndTorque,              "Use the newer 'sim.addForceAndTorque' notation",false},
    {"simAddForce",_simAddForce,                                "Use the newer 'sim.addForce' notation",false},
    {"simSetExplicitHandling",_simSetExplicitHandling,          "Use the newer 'sim.setExplicitHandling' notation",false},
    {"simGetExplicitHandling",_simGetExplicitHandling,          "Use the newer 'sim.getExplicitHandling' notation",false},
    {"simSetGraphUserData",_simSetGraphUserData,                "Use the newer 'sim.setGraphUserData' notation",false},
    {"simAddDrawingObject",_simAddDrawingObject,                "Use the newer 'sim.addDrawingObject' notation",false},
    {"simRemoveDrawingObject",_simRemoveDrawingObject,          "Use the newer 'sim.removeDrawingObject' notation",false},
    {"simAddDrawingObjectItem",_simAddDrawingObjectItem,        "Use the newer 'sim.addDrawingObjectItem' notation",false},
    {"simAddParticleObject",_simAddParticleObject,              "Use the newer 'sim.addParticleObject' notation",false},
    {"simRemoveParticleObject",_simRemoveParticleObject,        "Use the newer 'sim.removeParticleObject' notation",false},
    {"simAddParticleObjectItem",_simAddParticleObjectItem,      "Use the newer 'sim.addParticleObjectItem' notation",false},
    {"simGetObjectSizeFactor",_simGetObjectSizeFactor,          "Use the newer 'sim.getObjectSizeFactor' notation",false},
    {"simSetIntegerSignal",_simSetIntegerSignal,                "Use the newer 'sim.setIntegerSignal' notation",false},
    {"simGetIntegerSignal",_simGetIntegerSignal,                "Use the newer 'sim.getIntegerSignal' notation",false},
    {"simClearIntegerSignal",_simClearIntegerSignal,            "Use the newer 'sim.clearIntegerSignal' notation",false},
    {"simSetFloatSignal",_simSetFloatSignal,                    "Use the newer 'sim.setFloatSignal' notation",false},
    {"simGetFloatSignal",_simGetFloatSignal,                    "Use the newer 'sim.getFloatSignal' notation",false},
    {"simClearFloatSignal",_simClearFloatSignal,                "Use the newer 'sim.clearFloatSignal' notation",false},
    {"simSetStringSignal",_simSetStringSignal,                  "Use the newer 'sim.setStringSignal' notation",false},
    {"simGetStringSignal",_simGetStringSignal,                  "Use the newer 'sim.getStringSignal' notation",false},
    {"simClearStringSignal",_simClearStringSignal,              "Use the newer 'sim.clearStringSignal' notation",false},
    {"simGetSignalName",_simGetSignalName,                      "Use the newer 'sim.getSignalName' notation",false},
    {"simPersistentDataWrite",_simPersistentDataWrite,          "Use the newer 'sim.persistentDataWrite' notation",false},
    {"simPersistentDataRead",_simPersistentDataRead,            "Use the newer 'sim.persistentDataRead' notation",false},
    {"simSetObjectProperty",_simSetObjectProperty,              "Use the newer 'sim.setObjectProperty' notation",false},
    {"simGetObjectProperty",_simGetObjectProperty,              "Use the newer 'sim.getObjectProperty' notation",false},
    {"simSetObjectSpecialProperty",_simSetObjectSpecialProperty,"Use the newer 'sim.setObjectSpecialProperty' notation",false},
    {"simGetObjectSpecialProperty",_simGetObjectSpecialProperty,"Use the newer 'sim.getObjectSpecialProperty' notation",false},
    {"simSetModelProperty",_simSetModelProperty,                "Use the newer 'sim.setModelProperty' notation",false},
    {"simGetModelProperty",_simGetModelProperty,                "Use the newer 'sim.getModelProperty' notation",false},
    {"simReadForceSensor",_simReadForceSensor,                  "Use the newer 'sim.readForceSensor' notation",false},
    {"simBreakForceSensor",_simBreakForceSensor,                "Use the newer 'sim.breakForceSensor' notation",false},
    {"simGetLightParameters",_simGetLightParameters,            "Use the newer 'sim.getLightParameters' notation",false},
    {"simSetLightParameters",_simSetLightParameters,            "Use the newer 'sim.setLightParameters' notation",false},
    {"simGetLinkDummy",_simGetLinkDummy,                        "Use the newer 'sim.getLinkDummy' notation",false},
    {"simSetLinkDummy",_simSetLinkDummy,                        "Use the newer 'sim.setLinkDummy' notation",false},
    {"simSetShapeColor",_simSetShapeColor,                      "Use the newer 'sim.setShapeColor' notation",false},
    {"simGetShapeColor",_simGetShapeColor,                      "Use the newer 'sim.getShapeColor' notation",false},
    {"simResetDynamicObject",_simResetDynamicObject,            "Use the newer 'sim.resetDynamicObject' notation",false},
    {"simSetJointMode",_simSetJointMode,                        "Use the newer 'sim.setJointMode' notation",false},
    {"simGetJointMode",_simGetJointMode,                        "Use the newer 'sim.getJointMode' notation",false},
    {"simSerialSend",_simSerialSend,                            "Use the newer 'sim.serialSend' notation",false},
    {"simSerialCheck",_simSerialCheck,                          "Use the newer 'sim.serialCheck' notation",false},
    {"simGetContactInfo",_simGetContactInfo,                    "Use the newer 'sim.getContactInfo' notation",false},
    {"simAuxiliaryConsoleOpen",_simAuxiliaryConsoleOpen,        "Use the newer 'sim.auxiliaryConsoleOpen' notation",false},
    {"simAuxiliaryConsoleClose",_simAuxiliaryConsoleClose,      "Use the newer 'sim.auxiliaryConsoleClose' notation",false},
    {"simAuxiliaryConsolePrint",_simAuxiliaryConsolePrint,      "Use the newer 'sim.auxiliaryConsolePrint' notation",false},
    {"simAuxiliaryConsoleShow",_simAuxiliaryConsoleShow,        "Use the newer 'sim.auxiliaryConsoleShow' notation",false},
    {"simImportShape",_simImportShape,                          "Use the newer 'sim.importShape' notation",false},
    {"simImportMesh",_simImportMesh,                            "Use the newer 'sim.importMesh' notation",false},
    {"simExportMesh",_simExportMesh,                            "Use the newer 'sim.exportMesh' notation",false},
    {"simCreateMeshShape",_simCreateMeshShape,                  "Use the newer 'sim.createMeshShape' notation",false},
    {"simGetShapeMesh",_simGetShapeMesh,                        "Use the newer 'sim.getShapeMesh' notation",false},
    {"simCreatePureShape",_simCreatePureShape,                  "Use the newer 'sim.createPureShape' notation",false},
    {"simCreateHeightfieldShape",_simCreateHeightfieldShape,    "Use the newer 'sim.createHeightfieldShape' notation",false},
    {"simAddBanner",_simAddBanner,                              "Use the newer 'sim.addBanner' notation",false},
    {"simRemoveBanner",_simRemoveBanner,                        "Use the newer 'sim.removeBanner' notation",false},
    {"simCreateJoint",_simCreateJoint,                          "Use the newer 'sim.createJoint' notation",false},
    {"simCreateDummy",_simCreateDummy,                          "Use the newer 'sim.createDummy' notation",false},
    {"simCreateProximitySensor",_simCreateProximitySensor,      "Use the newer 'sim.createProximitySensor' notation",false},
    {"simCreateForceSensor",_simCreateForceSensor,              "Use the newer 'sim.createForceSensor' notation",false},
    {"simCreateVisionSensor",_simCreateVisionSensor,            "Use the newer 'sim.createVisionSensor' notation",false},
    {"simFloatingViewAdd",_simFloatingViewAdd,                  "Use the newer 'sim.floatingViewAdd' notation",false},
    {"simFloatingViewRemove",_simFloatingViewRemove,            "Use the newer 'sim.floatingViewRemove' notation",false},
    {"simAdjustView",_simAdjustView,                            "Use the newer 'sim.adjustView' notation",false},
    {"simCameraFitToView",_simCameraFitToView,                  "Use the newer 'sim.cameraFitToView' notation",false},
    {"simAnnounceSceneContentChange",_simAnnounceSceneContentChange,"Use the newer 'sim.announceSceneContentChange' notation",false},
    {"simGetObjectInt32Parameter",_simGetObjectInt32Parameter,  "Use the newer 'sim.getObjectInt32Parameter' notation",false},
    {"simSetObjectInt32Parameter",_simSetObjectInt32Parameter,  "Use the newer 'sim.setObjectInt32Parameter' notation",false},
    {"simGetObjectFloatParameter",_simGetObjectFloatParameter,  "Use the newer 'sim.getObjectFloatParameter' notation",false},
    {"simSetObjectFloatParameter",_simSetObjectFloatParameter,  "Use the newer 'sim.setObjectFloatParameter' notation",false},
    {"simGetObjectStringParameter",_simGetObjectStringParameter,"Use the newer 'sim.getObjectStringParameter' notation",false},
    {"simSetObjectStringParameter",_simSetObjectStringParameter,"Use the newer 'sim.setObjectStringParameter' notation",false},
    {"simGetRotationAxis",_simGetRotationAxis,                  "Use the newer 'sim.getRotationAxis' notation",false},
    {"simRotateAroundAxis",_simRotateAroundAxis,                "Use the newer 'sim.rotateAroundAxis' notation",false},
    {"simLaunchExecutable",_simLaunchExecutable,                "Use the newer 'sim.launchExecutable' notation",false},
    {"simGetJointForce",_simGetJointForce,                      "Use the newer 'sim.getJointForce' notation",false},
    {"simIsHandleValid",_simIsHandleValid,                      "Use the newer 'sim.isHandleValid' notation",false},
    {"simGetObjectQuaternion",_simGetObjectQuaternion,          "Use the newer 'sim.getObjectQuaternion' notation",false},
    {"simSetObjectQuaternion",_simSetObjectQuaternion,          "Use the newer 'sim.setObjectQuaternion' notation",false},
    {"simSetShapeMassAndInertia",_simSetShapeMassAndInertia,    "Deprecated. Use 'sim.setShapeMass' and/or 'sim.setShapeInertia' instead",false},
    {"simGetShapeMassAndInertia",_simGetShapeMassAndInertia,    "Deprecated. Use 'sim.getShapeMass' and/or 'sim.getShapeInertia' instead",false},
    {"simGroupShapes",_simGroupShapes,                          "Use the newer 'sim.groupShapes' notation",false},
    {"simUngroupShape",_simUngroupShape,                        "Use the newer 'sim.ungroupShape' notation",false},
    {"simConvexDecompose",_simConvexDecompose,                  "Use the newer 'sim.convexDecompose' notation",false},
    {"simAddGhost",_simAddGhost,                                "Use the newer 'sim.addGhost' notation",false},
    {"simModifyGhost",_simModifyGhost,                          "Use the newer 'sim.modifyGhost' notation",false},
    {"simQuitSimulator",_simQuitSimulator,                      "Use the newer 'sim.quitSimulator' notation",false},
    {"simGetThreadId",_simGetThreadId,                          "Use the newer 'sim.getThreadId' notation",false},
    {"simSetShapeMaterial",_simSetShapeMaterial,                "Use the newer 'sim.setShapeMaterial' notation",false},
    {"simGetTextureId",_simGetTextureId,                        "Use the newer 'sim.getTextureId' notation",false},
    {"simReadTexture",_simReadTexture,                          "Use the newer 'sim.readTexture' notation",false},
    {"simWriteTexture",_simWriteTexture,                        "Use the newer 'sim.writeTexture' notation",false},
    {"simCreateTexture",_simCreateTexture,                      "Use the newer 'sim.createTexture' notation",false},
    {"simWriteCustomDataBlock",_simWriteCustomDataBlock,        "Use the newer 'sim.writeCustomDataBlock' notation",false},
    {"simReadCustomDataBlock",_simReadCustomDataBlock,          "Use the newer 'sim.readCustomDataBlock' notation",false},
    {"simReadCustomDataBlockTags",_simReadCustomDataBlockTags,  "Use the newer 'sim.readCustomDataBlockTags' notation",false},
    {"simAddPointCloud",_simAddPointCloud,                      "Use the newer 'sim.addPointCloud' notation",false},
    {"simModifyPointCloud",_simModifyPointCloud,                "Use the newer 'sim.modifyPointCloud' notation",false},
    {"simGetShapeGeomInfo",_simGetShapeGeomInfo,                "Use the newer 'sim.getShapeGeomInfo' notation",false},
    {"simGetObjectsInTree",_simGetObjectsInTree,                "Use the newer 'sim.getObjectsInTree' notation",false},
    {"simSetObjectSizeValues",_simSetObjectSizeValues,          "Use the newer 'sim.setObjectSizeValues' notation",false},
    {"simGetObjectSizeValues",_simGetObjectSizeValues,          "Use the newer 'sim.getObjectSizeValues' notation",false},
    {"simScaleObject",_simScaleObject,                          "Use the newer 'sim.scaleObject' notation",false},
    {"simSetShapeTexture",_simSetShapeTexture,                  "Use the newer 'sim.setShapeTexture' notation",false},
    {"simGetShapeTextureId",_simGetShapeTextureId,              "Use the newer 'sim.getShapeTextureId' notation",false},
    {"simGetCollectionObjects",_simGetCollectionObjects,        "Use the newer 'sim.getCollectionObjects' notation",false},
    {"simHandleCustomizationScripts",_simHandleCustomizationScripts,"Use the newer 'sim.handleCustomizationScripts' notation",false},
    {"simSetScriptAttribute",_simSetScriptAttribute,            "Use the newer 'sim.setScriptAttribute' notation",false},
    {"simGetScriptAttribute",_simGetScriptAttribute,            "Use the newer 'sim.getScriptAttribute' notation",false},
    {"simHandleChildScripts",_simHandleChildScripts,            "Use the newer 'sim.handleChildScripts' notation",false},
    {"simReorientShapeBoundingBox",_simReorientShapeBoundingBox,"Use the newer 'sim.reorientShapeBoundingBox' notation",false},
    {"simHandleVisionSensor",_simHandleVisionSensor,            "Use the newer 'sim.handleVisionSensor' notation",false},
    {"simReadVisionSensor",_simReadVisionSensor,                "Use the newer 'sim.readVisionSensor' notation",false},
    {"simResetVisionSensor",_simResetVisionSensor,              "Use the newer 'sim.resetVisionSensor' notation",false},
    {"simGetVisionSensorResolution",_simGetVisionSensorResolution,"Use the newer 'sim.getVisionSensorResolution' notation",false},
    {"simGetVisionSensorImage",_simGetVisionSensorImage,        "Use the newer 'sim.getVisionSensorImage' notation",false},
    {"simSetVisionSensorImage",_simSetVisionSensorImage,        "Use the newer 'sim.setVisionSensorImage' notation",false},
    {"simGetVisionSensorCharImage",_simGetVisionSensorCharImage,"Use the newer 'sim.getVisionSensorCharImage' notation",false},
    {"simSetVisionSensorCharImage",_simSetVisionSensorCharImage,"Use the newer 'sim.setVisionSensorCharImage' notation",false},
    {"simGetVisionSensorDepthBuffer",_simGetVisionSensorDepthBuffer,"Use the newer 'sim.getVisionSensorDepthBuffer' notation",false},
    {"simCheckVisionSensor",_simCheckVisionSensor,              "Use the newer 'sim.checkVisionSensor' notation",false},
    {"simCheckVisionSensorEx",_simCheckVisionSensorEx,          "Use the newer 'sim.checkVisionSensorEx' notation",false},
    {"simRMLPos",_simRMLPos,                                    "Use the newer 'sim.rmlPos' notation",false},
    {"simRMLVel",_simRMLVel,                                    "Use the newer 'sim.rmlVel' notation",false},
    {"simRMLStep",_simRMLStep,                                  "Use the newer 'sim.rmlStep' notation",false},
    {"simRMLRemove",_simRMLRemove,                              "Use the newer 'sim.rmlRemove' notation",false},
    {"simBuildMatrixQ",_simBuildMatrixQ,                        "Use the newer 'sim.buildMatrixQ' notation",false},
    {"simGetQuaternionFromMatrix",_simGetQuaternionFromMatrix,  "Use the newer 'sim.getQuaternionFromMatrix' notation",false},
    {"simFileDialog",_simFileDialog,                            "Use the newer 'sim.fileDialog' notation",false},
    {"simMsgBox",_simMsgBox,                                    "Use the newer 'sim.msgBox' notation",false},
    {"simLoadModule",_simLoadModule,                            "Use the newer 'sim.loadModule' notation",false},
    {"simUnloadModule",_simUnloadModule,                        "Use the newer 'sim.unloadModule' notation",false},
    {"simCallScriptFunction",_simCallScriptFunction,            "Use the newer 'sim.callScriptFunction' notation",false},
    {"simGetExtensionString",_simGetExtensionString,            "Use the newer 'sim.getExtensionString' notation",false},
    {"simComputeMassAndInertia",_simComputeMassAndInertia,      "Use the newer 'sim.computeMassAndInertia' notation",false},
    {"simSetScriptVariable",_simSetScriptVariable,              "Use the newer 'sim.setScriptVariable' notation",false},
    {"simGetEngineFloatParameter",_simGetEngineFloatParameter,  "Use the newer 'sim.getEngineFloatParameter' notation",false},
    {"simGetEngineInt32Parameter",_simGetEngineInt32Parameter,  "Use the newer 'sim.getEngineInt32Parameter' notation",false},
    {"simGetEngineBoolParameter",_simGetEngineBoolParameter,    "Use the newer 'sim.getEngineBoolParameter' notation",false},
    {"simSetEngineFloatParameter",_simSetEngineFloatParameter,  "Use the newer 'sim.setEngineFloatParameter' notation",false},
    {"simSetEngineInt32Parameter",_simSetEngineInt32Parameter,  "Use the newer 'sim.setEngineInt32Parameter' notation",false},
    {"simSetEngineBoolParameter",_simSetEngineBoolParameter,    "Use the newer 'sim.setEngineBoolParameter' notation",false},
    {"simCreateOctree",_simCreateOctree,                        "Use the newer 'sim.createOctree' notation",false},
    {"simCreatePointCloud",_simCreatePointCloud,                "Use the newer 'sim.createPointCloud' notation",false},
    {"simSetPointCloudOptions",_simSetPointCloudOptions,        "Use the newer 'sim.setPointCloudOptions' notation",false},
    {"simGetPointCloudOptions",_simGetPointCloudOptions,        "Use the newer 'sim.getPointCloudOptions' notation",false},
    {"simInsertVoxelsIntoOctree",_simInsertVoxelsIntoOctree,    "Use the newer 'sim.insertVoxelsIntoOctree' notation",false},
    {"simRemoveVoxelsFromOctree",_simRemoveVoxelsFromOctree,    "Use the newer 'sim.removeVoxelsFromOctree' notation",false},
    {"simInsertPointsIntoPointCloud",_simInsertPointsIntoPointCloud,"Use the newer 'sim.insertPointsIntoPointCloud' notation",false},
    {"simRemovePointsFromPointCloud",_simRemovePointsFromPointCloud,"Use the newer 'sim.removePointsFromPointCloud' notation",false},
    {"simIntersectPointsWithPointCloud",_simIntersectPointsWithPointCloud,"Use the newer 'sim.intersectPointsWithPointCloud' notation",false},
    {"simGetOctreeVoxels",_simGetOctreeVoxels,                  "Use the newer 'sim.getOctreeVoxels' notation",false},
    {"simGetPointCloudPoints",_simGetPointCloudPoints,          "Use the newer 'sim.getPointCloudPoints' notation",false},
    {"simInsertObjectIntoOctree",_simInsertObjectIntoOctree,    "Use the newer 'sim.insertObjectIntoOctree' notation",false},
    {"simSubtractObjectFromOctree",_simSubtractObjectFromOctree,    "Use the newer 'sim.subtractObjectFromOctree' notation",false},
    {"simInsertObjectIntoPointCloud",_simInsertObjectIntoPointCloud,"Use the newer 'sim.insertObjectIntoPointCloud' notation",false},
    {"simSubtractObjectFromPointCloud",_simSubtractObjectFromPointCloud,    "Use the newer 'sim.subtractObjectFromPointCloud' notation",false},
    {"simCheckOctreePointOccupancy",_simCheckOctreePointOccupancy,"Use the newer 'sim.checkOctreePointOccupancy' notation",false},
    {"simOpenTextEditor",_simOpenTextEditor,                    "Deprecated. Use 'sim.textEditorOpen' instead",false},
    {"simSetVisionSensorFilter",_simSetVisionSensorFilter,      "Deprecated. Use vision callback functions instead",false},
    {"simGetVisionSensorFilter",_simGetVisionSensorFilter,      "Deprecated. Use vision callback functions instead",false},
    {"simGetScriptSimulationParameter",_simGetScriptSimulationParameter,"Deprecated. Use 'sim.getUserParameter' instead",false},
    {"simSetScriptSimulationParameter",_simSetScriptSimulationParameter,"Deprecated. Use 'sim.setUserParameter' instead",false},
    {"simHandleSimulationStart",_simHandleSimulationStart,      "Use the newer 'sim.handleSimulationStart' notation",false},
    {"simHandleSensingStart",_simHandleSensingStart,            "Use the newer 'sim.handleSensingStart' notation",false},
    {"simAuxFunc",_simAuxFunc,                                  "Use the newer 'sim.auxFunc' notation",false},
    {"simSetReferencedHandles",_simSetReferencedHandles,        "Use the newer 'sim.setReferencedHandles' notation",false},
    {"simGetReferencedHandles",_simGetReferencedHandles,        "Use the newer 'sim.getReferencedHandles' notation",false},
    {"simGetGraphCurve",_simGetGraphCurve,                        "Use the newer 'sim.getGraphCurve' notation",false},
    {"simTest",_simTest,                                        "Use the newer 'sim.test' notation",false},
    {"simAddStatusbarMessage",_simAddStatusbarMessage,          "Deprecated. Use 'sim.addLog' instead",false},
    // Following deprecated since 21/05/2017:
    {"simBoolOr16",_simBoolOr16,                                "Deprecated. Use sim.boolOr32 instead",false},
    {"simBoolAnd16",_simBoolAnd16,                              "Deprecated. Use sim.boolAnd32 instead",false},
    {"simBoolXor16",_simBoolXor16,                              "Deprecated. Use sim.boolXor32 instead",false},
    {"simGetObjectSelectionSize",_simGetObjectSelectionSize,    "Deprecated. Use sim.getObjectSelection instead",false},
    {"simGetObjectLastSelection",_simGetObjectLastSelection,    "Deprecated. Use sim.getObjectSelection instead",false},
    {"simReleaseScriptRawBuffer",_simReleaseScriptRawBuffer,    "Deprecated",false},
    // Following deprecated since V3.3.0:
    {"simGetPathPlanningHandle",_simGetPathPlanningHandle,      "Deprecated. Use the OMPL-based API instead",false},
    {"simSearchPath",_simSearchPath,                            "Deprecated. Use the OMPL-based API instead",false},
    {"simInitializePathSearch",_simInitializePathSearch,        "Deprecated. Use the OMPL-based API instead",false},
    {"simPerformPathSearchStep",_simPerformPathSearchStep,      "Deprecated. Use the OMPL-based API instead",false},
    // Following deprecated since 09/02/2017:
    {"simLoadUI",_simLoadUI,                                    "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSaveUI",_simSaveUI,                                    "Deprecated. Use Qt-based custom UIs instead",false},
    {"simRemoveUI",_simRemoveUI,                                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreateUI",_simCreateUI,                                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreateUIButton",_simCreateUIButton,                    "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIPosition",_simGetUIPosition,                      "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIPosition",_simSetUIPosition,                      "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIHandle",_simGetUIHandle,                          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIProperty",_simGetUIProperty,                      "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIEventButton",_simGetUIEventButton,                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIProperty",_simSetUIProperty,                      "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIButtonProperty",_simGetUIButtonProperty,          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIButtonProperty",_simSetUIButtonProperty,          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIButtonSize",_simGetUIButtonSize,                  "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIButtonLabel",_simSetUIButtonLabel,                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUIButtonLabel",_simGetUIButtonLabel,                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUISlider",_simSetUISlider,                          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGetUISlider",_simGetUISlider,                          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIButtonColor",_simSetUIButtonColor,                "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreateUIButtonArray",_simCreateUIButtonArray,          "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSetUIButtonArrayColor",_simSetUIButtonArrayColor,      "Deprecated. Use Qt-based custom UIs instead",false},
    {"simDeleteUIButtonArray",_simDeleteUIButtonArray,          "Deprecated. Use Qt-based custom UIs instead",false},
    // Following for backward compatibility (nov-dec 2011):
    {"simGet2DElementHandle",_simGetUIHandle,                       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementProperty",_simGetUIProperty,                   "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementEventButton",_simGetUIEventButton,             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementProperty",_simSetUIProperty,                   "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementButtonProperty",_simGetUIButtonProperty,       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementButtonProperty",_simSetUIButtonProperty,       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementButtonSize",_simGetUIButtonSize,               "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementButtonLabel",_simSetUIButtonLabel,             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementButtonLabel",_simGetUIButtonLabel,             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementSlider",_simSetUISlider,                       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementSlider",_simGetUISlider,                       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementButtonColor",_simSetUIButtonColor,             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreate2DElementButtonArray",_simCreateUIButtonArray,       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementButtonArrayColor",_simSetUIButtonArrayColor,   "Deprecated. Use Qt-based custom UIs instead",false},
    {"simDelete2DElementButtonArray",_simDeleteUIButtonArray,       "Deprecated. Use Qt-based custom UIs instead",false},
    {"simRemove2DElement",_simRemoveUI,                             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreate2DElement",_simCreateUI,                             "Deprecated. Use Qt-based custom UIs instead",false},
    {"simCreate2DElementButton",_simCreateUIButton,                 "Deprecated. Use Qt-based custom UIs instead",false},
    {"simGet2DElementPosition",_simGetUIPosition,                   "Deprecated. Use Qt-based custom UIs instead",false},
    {"simSet2DElementPosition",_simSetUIPosition,                   "Deprecated. Use Qt-based custom UIs instead",false},
    // Following deprecated since 26/12/2016:
    {"simAddSceneCustomData",_simAddSceneCustomData,                "Deprecated. Use sim.writeCustomDataBlock instead",false},
    {"simGetSceneCustomData",_simGetSceneCustomData,                "Deprecated. Use sim.readCustomDataBlock instead",false},
    {"simAddObjectCustomData",_simAddObjectCustomData,              "Deprecated. Use sim.writeCustomDataBlock instead",false},
    {"simGetObjectCustomData",_simGetObjectCustomData,              "Deprecated. Use sim.readCustomDataBlock instead",false},
    // Following deprecated since 29/10/2016:
    {"simGetMaterialId",_simGetMaterialId,                          "Deprecated. Use sim.setShapeMaterial instead",false},
    {"simGetShapeMaterial",_simGetShapeMaterial,                    "Deprecated. Use sim.setShapeMaterial instead",false},
    {"simHandleVarious",_simHandleVarious,                          "Deprecated. Use sim.handleSimulationStart and sim.handleSensingStart instead",false},
    // Following deprecated since 13/9/2016:
    {"simPackInts",_simPackInt32Table,                              "Deprecated. Use sim.packInt32Table instead",false},
    {"simPackUInts",_simPackUInt32Table,                            "Deprecated. Use sim.packUInt32Table instead",false},
    {"simPackFloats",_simPackFloatTable,                            "Deprecated. Use sim.packFloatTable instead",false},
    {"simPackDoubles",_simPackDoubleTable,                          "Deprecated. Use sim.packDoubleTable instead",false},
    {"simPackBytes",_simPackUInt8Table,                             "Deprecated. Use sim.packUInt8Table instead",false},
    {"simPackWords",_simPackUInt16Table,                            "Deprecated. Use sim.packUInt16Table instead",false},
    {"simUnpackInts",_simUnpackInt32Table,                          "Deprecated. Use sim.unpackInt32Table instead",false},
    {"simUnpackUInts",_simUnpackUInt32Table,                        "Deprecated. Use sim.unpackUInt32Table instead",false},
    {"simUnpackFloats",_simUnpackFloatTable,                        "Deprecated. Use sim.unpackFloatTable instead",false},
    {"simUnpackDoubles",_simUnpackDoubleTable,                      "Deprecated. Use sim.unpackDoubleTable instead",false},
    {"simUnpackBytes",_simUnpackUInt8Table,                         "Deprecated. Use sim.unpackUInt8Table instead",false},
    {"simUnpackWords",_simUnpackUInt16Table,                        "Deprecated. Use sim.unpackUInt16Table instead",false},
    // Following deprecated:
    {"simGetInvertedMatrix",_simGetInvertedMatrix,                  "Deprecated. Use sim.invertMatrix instead",false}, // 10/05/2016
    {"simEnableWorkThreads",_simEnableWorkThreads,                  "Deprecated. Has no effect",false},
    {"simWaitForWorkThreads",_simWaitForWorkThreads,                "Deprecated. Has no effect",false},
    {"simFindIkPath",_simFindIkPath,                                "Deprecated. Use sim.generateIkPath instead",false},
    {"simHandleChildScript",_simHandleChildScript,                  "Deprecated. Use sim.handleChildScripts instead",false},
    {"simHandleSensingChildScripts",_simHandleSensingChildScripts,  "Deprecated. Use sim.handleChildScripts instead",false},
    {"simDelegateChildScriptExecution",_simDelegateChildScriptExecution,"Deprecated. Has no effect",false},
    {"simResetTracing",_simResetTracing,                            "Deprecated. Has no effect",false},
    {"simHandleTracing",_simHandleTracing,                          "Deprecated. Has no effect",false},
    {"simCopyPasteSelectedObjects",_simCopyPasteSelectedObjects,    "Deprecated. Use sim.copyPasteObjects instead",false},
    {"simGetShapeVertex",_simGetShapeVertex,                        "Deprecated. Use sim.getShapeMesh instead",false},
    {"simGetShapeTriangle",_simGetShapeTriangle,                    "Deprecated. Use sim.getShapeMesh instead",false},
    {"simGetInstanceIndex",_simGetInstanceIndex,                    "Deprecated. Returns 0",false},
    {"simGetVisibleInstanceIndex",_simGetVisibleInstanceIndex,      "Deprecated. Returns 0",false},
    {"simGetSystemTimeInMilliseconds",_simGetSystemTimeInMilliseconds,"Deprecated. Use sim.getSystemTimeInMs instead",false},
    {"simLockInterface",_simLockInterface,                          "Deprecated. Has no effect",false},
    {"simJointGetForce",_simJointGetForce,                          "Deprecated. Use sim.getJointForce instead",false},
    {"simScaleSelectedObjects",_simScaleSelectedObjects,            "Deprecated. Use sim.scaleObjects instead",false},
    {"simDeleteSelectedObjects",_simDeleteSelectedObjects,          "Deprecated. Use sim.removeObject instead",false},
    {"simResetPath",_simResetPath,                                  "Deprecated",false},
    {"simResetJoint",_simResetJoint,                                "Deprecated",false},
    {"simGetMpConfigForTipPose",_simGetMpConfigForTipPose,          "Deprecated. Use sim.getConfigForTipPose instead",false},
    // Following for backward compatibility (June 2020):
    {"simGetNameSuffix",_simGetNameSuffix,                      "Deprecated.",false},
    {"simSetNameSuffix",_simSetNameSuffix,                      "Deprecated.",false},
    // Following for backward compatibility (Dec 2015):
    {"simSetBooleanParameter",_simSetBoolParameter,                 "Deprecated. Use sim.setBoolParameter instead",false},
    {"simGetBooleanParameter",_simGetBoolParameter,                 "Deprecated. Use sim.getBoolParameter instead",false},
    {"simSetIntegerParameter",_simSetInt32Parameter,                "Deprecated. Use sim.setInt32Parameter instead",false},
    {"simGetIntegerParameter",_simGetInt32Parameter,                "Deprecated. Use sim.getInt32Parameter instead",false},
    {"simSetFloatingParameter",_simSetFloatParameter,               "Deprecated. Use sim.setFloatParameter instead",false},
    {"simGetFloatingParameter",_simGetFloatParameter,               "Deprecated. Use sim.getFloatParameter instead",false},
    {"simGetObjectIntParameter",_simGetObjectInt32Parameter,        "Deprecated. Use sim.getObjectInt32Parameter instead",false},
    {"simSetObjectIntParameter",_simSetObjectInt32Parameter,        "Deprecated. Use sim.setObjectInt32Parameter instead",false},
    // Following for backward compatibility:
    {"simHandleRenderingSensor",_simHandleVisionSensor,             "Deprecated. Use sim.handleVisionSensor instead",false},
    {"simReadRenderingSensor",_simReadVisionSensor,                 "Deprecated. Use sim.readVisionSensor instead",false},
    {"simResetRenderingSensor",_simResetVisionSensor,               "Deprecated. Use sim.resetVisionSensor instead",false},
    {"simGetRenderingSensorResolution",_simGetVisionSensorResolution,"Deprecated. Use sim.getVisionSensorResolution instead",false},
    {"simGetRenderingSensorImage",_simGetVisionSensorImage,         "Deprecated. Use sim.getVisionSensorImage instead",false},
    {"simSetRenderingSensorImage",_simSetVisionSensorImage,         "Deprecated. Use sim.setVisionSensorImage instead",false},
    {"simGetRenderingSensorDepthBuffer",_simGetVisionSensorDepthBuffer,"Deprecated. Use sim.getVisionSensorDepthBuffer instead",false},
    {"simCheckRenderingSensor",_simCheckVisionSensor,               "Deprecated. Use sim.checkVisionSensor instead",false},
    {"simCheckRenderingSensorEx",_simCheckVisionSensorEx,           "Deprecated. Use sim.checkVisionSensorEx instead",false},
    // Following for backward compatibility (deprecated since 23/5/2014):
    {"simRMLPosition",_simRMLPosition,                              "Deprecated. Use sim.rmlPos instead",false},
    {"simRMLVelocity",_simRMLVelocity,                              "Deprecated. Use sim.rmlVel instead",false},

    {"simCheckIkGroup",_simCheckIkGroup,                        "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simCreateIkGroup",_simCreateIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simRemoveIkGroup",_simRemoveIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simCreateIkElement",_simCreateIkElement,                  "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simExportIk",_simExportIk,                                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simComputeJacobian",_simComputeJacobian,                  "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simGetConfigForTipPose",_simGetConfigForTipPose,          "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simGenerateIkPath",_simGenerateIkPath,                    "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simGetIkGroupHandle",_simGetIkGroupHandle,                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simGetIkGroupMatrix",_simGetIkGroupMatrix,                "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simHandleIkGroup",_simHandleIkGroup,                      "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simSetIkGroupProperties",_simSetIkGroupProperties,        "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simSetIkElementProperties",_simSetIkElementProperties,    "Deprecated. Use the corresponding API function from the kinematics plugin instead",false},
    {"simTubeRead",_simTubeRead,                                "Deprecated. Use signals or custom data blocks instead",false},
    {"simTubeOpen",_simTubeOpen,                                "Deprecated. Use signals or custom data blocks instead",false},
    {"simTubeClose",_simTubeClose,                              "Deprecated. Use signals or custom data blocks instead",false},
    {"simTubeWrite",_simTubeWrite,                              "Deprecated. Use signals or custom data blocks instead",false},
    {"simTubeStatus",_simTubeStatus,                            "Deprecated. Use signals or custom data blocks instead",false},
    {"simSendData",_simSendData,                                "Deprecated. Use signals or custom data blocks instead",false},
    {"simReceiveData",_simReceiveData,                          "Deprecated. Use signals or custom data blocks instead",false},
    {"simMoveToPosition",_simMoveToPosition,                    "Deprecated. Use 'sim.moveToPose' instead",false},
    {"simMoveToObject",_simMoveToObject,                        "Deprecated. Use 'sim.moveToPose' instead",false},
    {"simFollowPath",_simFollowPath,                            "Deprecated. Use 'sim.rmlPos' and 'sim.rmlStep' instead",false},
    {"simMoveToJointPositions",_simMoveToJointPositions,        "Deprecated. Use 'sim.moveToConfig' instead",false},
    {"simGetPathPosition",_simGetPathPosition,                  "Deprecated",false},
    {"simSetPathPosition",_simSetPathPosition,                  "Deprecated",false},
    {"simGetPathLength",_simGetPathLength,                      "Deprecated",false},
    {"simGetDataOnPath",_simGetDataOnPath,                      "Deprecated",false},
    {"simGetPositionOnPath",_simGetPositionOnPath,              "Deprecated",false},
    {"simGetOrientationOnPath",_simGetOrientationOnPath,        "Deprecated",false},
    {"simGetClosestPositionOnPath",_simGetClosestPositionOnPath,"Deprecated",false},
    {"simCreatePath",_simCreatePath,                            "Deprecated",false},
    {"simInsertPathCtrlPoints",_simInsertPathCtrlPoints,        "Deprecated",false},
    {"simCutPathCtrlPoints",_simCutPathCtrlPoints,              "Deprecated",false},
    {"simGetScriptExecutionCount",_simGetScriptExecutionCount,  "Deprecated",false},
    {"simIsScriptExecutionThreaded",_simIsScriptExecutionThreaded,"Deprecated",false},
    {"simIsScriptRunningInThread",_simIsScriptRunningInThread,  "Deprecated",false},
    {"simSetThreadResumeLocation",_simSetThreadResumeLocation,  "Deprecated",false},
    {"simResumeThreads",_simResumeThreads,                      "Deprecated",false},
    {"simLaunchThreadedChildScripts",_simLaunchThreadedChildScripts,"Deprecated",false},
    //{"simRMLMoveToPosition",_simRMLMoveToPosition,              "Deprecated. Use 'sim.moveToPose' instead",false},
    //{"simRMLMoveToJointPositions",_simRMLMoveToJointPositions,  "Deprecated. Use 'sim.moveToConfig' instead",false},
    //{"simWait",_simWait,                                        "Use the newer 'sim.wait' notation",false},
    //{"simWaitForSignal",_simWaitForSignal,                      "Use the newer 'sim.waitForSignal' notation",false},
    //{"simSetThreadIsFree",_simSetThreadIsFree,                  "Use the newer 'sim.setThreadIsFree' notation",false},
    //{"simSerialRead",_simSerialRead,                            "Use the newer 'sim.serialRead' notation",false},
    //{"simSerialOpen",_simSerialOpen,                            "Use the newer 'sim.serialOpen' notation",false},
    //{"simSerialClose",_simSerialClose,                          "Use the newer 'sim.serialClose' notation",false},
    //{"simSerialPortOpen",_simSerialPortOpen,                        "Deprecated. Use sim.serialOpen instead",false},
    //{"simSerialPortClose",_simSerialPortClose,                      "Deprecated. Use sim.serialClose instead",false},
    //{"simSerialPortSend",_simSerialPortSend,                        "Deprecated. Use sim.serialSend instead",false},
    //{"simSerialPortRead",_simSerialPortRead,                        "Deprecated. Use sim.serialRead instead",false},


    {"",nullptr,"",false}
};

const SLuaVariables simLuaVariables[]=
{
    // 3D object types (main types):
    {"sim.object_shape_type",sim_object_shape_type,true},
    {"sim.object_joint_type",sim_object_joint_type,true},
    {"sim.object_graph_type",sim_object_graph_type,true},
    {"sim.object_camera_type",sim_object_camera_type,true},
    {"sim.object_dummy_type",sim_object_dummy_type,true},
    {"sim.object_proximitysensor_type",sim_object_proximitysensor_type,true},
    {"sim.object_path_type",sim_object_path_type,true},
    {"sim.object_renderingsensor_type",sim_object_visionsensor_type,true},
    {"sim.object_visionsensor_type",sim_object_visionsensor_type,true},
    {"sim.object_mill_type",sim_object_mill_type,true},
    {"sim.object_forcesensor_type",sim_object_forcesensor_type,true},
    {"sim.object_light_type",sim_object_light_type,true},
    {"sim.object_mirror_type",sim_object_mirror_type,true},
    {"sim.object_octree_type",sim_object_octree_type,true},
    {"sim.object_pointcloud_type",sim_object_pointcloud_type,true},
    // 3D object sub-types:
    {"sim.light_omnidirectional_subtype",sim_light_omnidirectional_subtype,true},
    {"sim.light_spot_subtype",sim_light_spot_subtype,true},
    {"sim.light_directional_subtype",sim_light_directional_subtype,true},
    {"sim.joint_revolute_subtype",sim_joint_revolute_subtype,true},
    {"sim.joint_prismatic_subtype",sim_joint_prismatic_subtype,true},
    {"sim.joint_spherical_subtype",sim_joint_spherical_subtype,true},
    {"sim.shape_simpleshape_subtype",sim_shape_simpleshape_subtype,true},
    {"sim.shape_multishape_subtype",sim_shape_multishape_subtype,true},
    {"sim.proximitysensor_pyramid_subtype",sim_proximitysensor_pyramid_subtype,true},
    {"sim.proximitysensor_cylinder_subtype",sim_proximitysensor_cylinder_subtype,true},
    {"sim.proximitysensor_disc_subtype",sim_proximitysensor_disc_subtype,true},
    {"sim.proximitysensor_cone_subtype",sim_proximitysensor_cone_subtype,true},
    {"sim.proximitysensor_ray_subtype",sim_proximitysensor_ray_subtype,true},
    {"sim.mill_pyramid_subtype",sim_mill_pyramid_subtype,true},
    {"sim.mill_cylinder_subtype",sim_mill_cylinder_subtype,true},
    {"sim.mill_disc_subtype",sim_mill_disc_subtype,true},
    {"sim.mill_cone_subtype",sim_mill_cone_subtype,true},
    {"sim.object_no_subtype",sim_object_no_subtype,true},
    // Other object types:
    {"sim.appobj_object_type",sim_appobj_object_type,true},
    {"sim.appobj_collision_type",sim_appobj_collision_type,true},
    {"sim.appobj_distance_type",sim_appobj_distance_type,true},
    {"sim.appobj_simulation_type",sim_appobj_simulation_type,true},
    {"sim.appobj_collection_type",sim_appobj_collection_type,true},
    {"sim.appobj_script_type",sim_appobj_script_type,true},
    {"sim.appobj_texture_type",sim_appobj_texture_type,true},
    // Simulation messages:
    {"sim.message_ui_button_state_change",sim_message_ui_button_state_change,true},
    {"sim.message_model_loaded",sim_message_model_loaded,true},
    {"sim.message_scene_loaded",sim_message_scene_loaded,true},
    {"sim.message_object_selection_changed",sim_message_object_selection_changed,true},
    {"sim.message_keypress",sim_message_keypress,true},
    {"sim.message_bannerclicked",sim_message_bannerclicked,true},
    {"sim.message_prox_sensor_select_down",sim_message_prox_sensor_select_down,true},
    {"sim.message_prox_sensor_select_up",sim_message_prox_sensor_select_up,true},
    {"sim.message_pick_select_down",sim_message_pick_select_down,true},
    // Button properties:
    {"sim.buttonproperty_button",sim_buttonproperty_button,true},
    {"sim.buttonproperty_label",sim_buttonproperty_label,true},
    {"sim.buttonproperty_editbox",sim_buttonproperty_editbox,true},
    {"sim.buttonproperty_slider",sim_buttonproperty_slider,true},
    {"sim.buttonproperty_staydown",sim_buttonproperty_staydown,true},
    {"sim.buttonproperty_enabled",sim_buttonproperty_enabled,true},
    {"sim.buttonproperty_borderless",sim_buttonproperty_borderless,true},
    {"sim.buttonproperty_horizontallycentered",sim_buttonproperty_horizontallycentered,true},
    {"sim.buttonproperty_ignoremouse",sim_buttonproperty_ignoremouse,true},
    {"sim.buttonproperty_isdown",sim_buttonproperty_isdown,true},
    {"sim.buttonproperty_transparent",sim_buttonproperty_transparent,true},
    {"sim.buttonproperty_nobackgroundcolor",sim_buttonproperty_nobackgroundcolor,true},
    {"sim.buttonproperty_rollupaction",sim_buttonproperty_rollupaction,true},
    {"sim.buttonproperty_closeaction",sim_buttonproperty_closeaction,true},
    {"sim.buttonproperty_verticallycentered",sim_buttonproperty_verticallycentered,true},
    {"sim.buttonproperty_downupevent",sim_buttonproperty_downupevent,true},
    // object properties. Combine with the | operator
    {"sim.objectproperty_collapsed",sim_objectproperty_collapsed,true},
    {"sim.objectproperty_selectable",sim_objectproperty_selectable,true},
    {"sim.objectproperty_selectmodelbaseinstead",sim_objectproperty_selectmodelbaseinstead,true},
    {"sim.objectproperty_dontshowasinsidemodel",sim_objectproperty_dontshowasinsidemodel,true},
    {"sim.objectproperty_canupdatedna",sim_objectproperty_canupdatedna,true},
    {"sim.objectproperty_selectinvisible",sim_objectproperty_selectinvisible,true},
    {"sim.objectproperty_depthinvisible",sim_objectproperty_depthinvisible,true},
    {"sim.objectproperty_cannotdelete",sim_objectproperty_cannotdelete,true},
    {"sim.objectproperty_cannotdeleteduringsim",sim_objectproperty_cannotdeleteduringsim,true},
    {"sim.objectproperty_hierarchyhiddenmodelchild",sim_objectproperty_hierarchyhiddenmodelchild,true},
    // Simulation status:
    {"sim.simulation_stopped",sim_simulation_stopped,true},
    {"sim.simulation_paused",sim_simulation_paused,true},
    {"sim.simulation_advancing",sim_simulation_advancing,true},
    {"sim.simulation_advancing_firstafterstop",sim_simulation_advancing_firstafterstop,true},
    {"sim.simulation_advancing_running",sim_simulation_advancing_running,true},
    {"sim.simulation_advancing_lastbeforepause",sim_simulation_advancing_lastbeforepause,true},
    {"sim.simulation_advancing_firstafterpause",sim_simulation_advancing_firstafterpause,true},
    {"sim.simulation_advancing_abouttostop",sim_simulation_advancing_abouttostop,true},
    {"sim.simulation_advancing_lastbeforestop",sim_simulation_advancing_lastbeforestop,true},
    // Texture mapping modes:
    {"sim.texturemap_plane",sim_texturemap_plane,true},
    {"sim.texturemap_cylinder",sim_texturemap_cylinder,true},
    {"sim.texturemap_sphere",sim_texturemap_sphere,true},
    {"sim.texturemap_cube",sim_texturemap_cube,true},
    // script types:
    {"sim.scripttype_mainscript",sim_scripttype_mainscript,true},
    {"sim.scripttype_childscript",sim_scripttype_childscript,true},
    {"sim.scripttype_addonscript",sim_scripttype_addonscript,true},
    {"sim.scripttype_addonfunction",sim_scripttype_addonfunction,true},
    {"sim.scripttype_customizationscript",sim_scripttype_customizationscript,true},
    {"sim.scripttype_sandboxscript",sim_scripttype_sandboxscript,true},
    // System callbacks
    {"sim.syscb_init",sim_syscb_init,true},
    {"sim.syscb_cleanup",sim_syscb_cleanup,true},
    {"sim.syscb_nonsimulation",sim_syscb_nonsimulation,true},
    {"sim.syscb_beforemainscript",sim_syscb_beforemainscript,true},
    {"sim.syscb_beforesimulation",sim_syscb_beforesimulation,true},
    {"sim.syscb_aftersimulation",sim_syscb_aftersimulation,true},
    {"sim.syscb_actuation",sim_syscb_actuation,true},
    {"sim.syscb_sensing",sim_syscb_sensing,true},
    {"sim.syscb_suspended",sim_syscb_suspended,true},
    {"sim.syscb_suspend",sim_syscb_suspend,true},
    {"sim.syscb_resume",sim_syscb_resume,true},
    {"sim.syscb_beforeinstanceswitch",sim_syscb_beforeinstanceswitch,true},
    {"sim.syscb_afterinstanceswitch",sim_syscb_afterinstanceswitch,true},
    {"sim.syscb_beforecopy",sim_syscb_beforecopy,true},
    {"sim.syscb_aftercopy",sim_syscb_aftercopy,true},
    {"sim.syscb_aos_suspend",sim_syscb_aos_suspend,true},
    {"sim.syscb_aos_resume",sim_syscb_aos_resume,true},
    {"sim.syscb_jointcallback",sim_syscb_jointcallback,true},
    {"sim.syscb_vision",sim_syscb_vision,true},
    {"sim.syscb_trigger",sim_syscb_trigger,true},
    {"sim.syscb_contactcallback",sim_syscb_contactcallback,true},
    {"sim.syscb_dyncallback",sim_syscb_dyncallback,true},
    {"sim.syscb_customcallback1",sim_syscb_customcallback1,true},
    {"sim.syscb_customcallback2",sim_syscb_customcallback2,true},
    {"sim.syscb_customcallback3",sim_syscb_customcallback3,true},
    {"sim.syscb_customcallback4",sim_syscb_customcallback4,true},
    {"sim.syscb_beforedelete",sim_syscb_beforedelete,true},
    {"sim.syscb_afterdelete",sim_syscb_afterdelete,true},
    {"sim.syscb_aftercreate",sim_syscb_aftercreate,true},
    {"sim.syscb_threadmain",sim_syscb_threadmain,true},
    {"sim.syscb_userconfig",sim_syscb_userconfig,true},
    {"sim.syscb_br",sim_syscb_br,true},
    {"sim.syscb_xr",sim_syscb_xr,true},
    // script attributes:
    {"sim.customizationscriptattribute_activeduringsimulation",sim_customizationscriptattribute_activeduringsimulation,true},
    {"sim.scriptattribute_executionorder",sim_scriptattribute_executionorder,true},
    {"sim.scriptattribute_executioncount",sim_scriptattribute_executioncount,true},
    {"sim.childscriptattribute_automaticcascadingcalls",sim_childscriptattribute_automaticcascadingcalls,true},
    {"sim.childscriptattribute_enabled",sim_childscriptattribute_enabled,true},
    {"sim.scriptattribute_enabled",sim_scriptattribute_enabled,true},
    {"sim.customizationscriptattribute_cleanupbeforesave",sim_customizationscriptattribute_cleanupbeforesave,true},
    {"sim.scriptattribute_debuglevel",sim_scriptattribute_debuglevel,true},
    {"sim.scriptattribute_scripttype",sim_scriptattribute_scripttype,true},
    // script execution order:
    {"sim.scriptexecorder_first",sim_scriptexecorder_first,true},
    {"sim.scriptexecorder_normal",sim_scriptexecorder_normal,true},
    {"sim.scriptexecorder_last",sim_scriptexecorder_last,true},
    // script debug level:
    {"sim.scriptdebug_none",sim_scriptdebug_none,true},
    {"sim.scriptdebug_syscalls",sim_scriptdebug_syscalls,true},
    {"sim.scriptdebug_vars_interval",sim_scriptdebug_vars_interval,true},
    {"sim.scriptdebug_allcalls",sim_scriptdebug_allcalls,true},
    {"sim.scriptdebug_vars",sim_scriptdebug_vars,true},
    {"sim.scriptdebug_callsandvars",sim_scriptdebug_callsandvars,true},
    // threaded script resume location:
    {"sim.scriptthreadresume_allnotyetresumed",sim_scriptthreadresume_allnotyetresumed,true},
    {"sim.scriptthreadresume_default",sim_scriptthreadresume_default,true},
    {"sim.scriptthreadresume_actuation_first",sim_scriptthreadresume_actuation_first,true},
    {"sim.scriptthreadresume_actuation_last",sim_scriptthreadresume_actuation_last,true},
    {"sim.scriptthreadresume_sensing_first",sim_scriptthreadresume_sensing_first,true},
    {"sim.scriptthreadresume_sensing_last",sim_scriptthreadresume_sensing_last,true},
    {"sim.scriptthreadresume_custom",sim_scriptthreadresume_custom,true},
    // General callback IDs:
    {"sim.callbackid_rossubscriber",sim_callbackid_rossubscriber,true},
    {"sim.callbackid_dynstep",sim_callbackid_dynstep,true},
    {"sim.callbackid_userdefined",sim_callbackid_userdefined,true},
    // Script execution result:
    {"sim.script_no_error",sim_script_no_error,true},
    {"sim.script_main_script_nonexistent",sim_script_main_script_nonexistent,true},
    {"sim.script_main_not_called",sim_script_main_script_not_called,true},
    {"sim.script_reentrance_error",sim_script_reentrance_error,true},
    {"sim.script_lua_error",sim_script_lua_error,true},
    {"sim.script_call_error",sim_script_call_error,true},
    // API call error messages
    {"sim.api_error_report",sim_api_error_report,true},
    {"sim.api_error_output",sim_api_error_output,true},
    {"sim.api_warning_output",sim_api_warning_output,true},
    // special arguments for some functions:
    {"sim.handle_all",sim_handle_all,true},
    {"sim.handle_all_except_explicit",sim_handle_all_except_explicit,true},
    {"sim.handle_self",sim_handle_self,true},
    {"sim.handle_main_script",sim_handle_main_script,true},
    {"sim.handle_tree",sim_handle_tree,true},
    {"sim.handle_chain",sim_handle_chain,true},
    {"sim.handle_single",sim_handle_single,true},
    {"sim.handle_default",sim_handle_default,true},
    {"sim.handle_all_except_self",sim_handle_all_except_self,true},
    {"sim.handle_parent",sim_handle_parent,true},
    {"sim.handle_scene",sim_handle_scene,true},
    {"sim.handle_app",sim_handle_app,true},
    // special handle flags:
    {"sim.handleflag_assembly",sim_handleflag_assembly,true},
    {"sim.handleflag_camera",sim_handleflag_camera,true},
    {"sim.handleflag_togglevisibility",sim_handleflag_togglevisibility,true},
    {"sim.handleflag_extended",sim_handleflag_extended,true},
    {"sim.handleflag_greyscale",sim_handleflag_greyscale,true},
    {"sim.handleflag_depthbuffermeters",sim_handleflag_depthbuffermeters,true},
    {"sim.handleflag_depthbuffer",sim_handleflag_depthbuffer,true},
    {"sim.handleflag_keeporiginal",sim_handleflag_keeporiginal,true},
    {"sim.handleflag_codedstring",sim_handleflag_codedstring,true},
    {"sim.handleflag_wxyzquaternion",sim_handleflag_wxyzquaternion,true},
    {"sim.handleflag_reljointbaseframe",sim_handleflag_reljointbaseframe,true},
    {"sim.handleflag_abscoords",sim_handleflag_abscoords,true},
    {"sim.handleflag_model",sim_handleflag_model,true},
    {"sim.handleflag_rawvalue",sim_handleflag_rawvalue,true},
    {"sim.handleflag_altname",sim_handleflag_altname,true},
    {"sim.handleflag_axis",sim_handleflag_axis,true},
    {"sim.handleflag_resetforce",sim_handleflag_resetforce,true},
    {"sim.handleflag_resettorque",sim_handleflag_resettorque,true},
    {"sim.handleflag_resetforcetorque",sim_handleflag_resetforcetorque,true},
    {"sim.handleflag_silenterror",sim_handleflag_silenterror,true},
    // General object main properties:
    {"sim.objectspecialproperty_collidable",sim_objectspecialproperty_collidable,true},
    {"sim.objectspecialproperty_measurable",sim_objectspecialproperty_measurable,true},
    {"sim.objectspecialproperty_detectable_ultrasonic",sim_objectspecialproperty_detectable_ultrasonic,true},
    {"sim.objectspecialproperty_detectable_infrared",sim_objectspecialproperty_detectable_infrared,true},
    {"sim.objectspecialproperty_detectable_laser",sim_objectspecialproperty_detectable_laser,true},
    {"sim.objectspecialproperty_detectable_inductive",sim_objectspecialproperty_detectable_inductive,true},
    {"sim.objectspecialproperty_detectable_capacitive",sim_objectspecialproperty_detectable_capacitive,true},
    {"sim.objectspecialproperty_renderable",sim_objectspecialproperty_renderable,true},
    {"sim.objectspecialproperty_detectable_all",sim_objectspecialproperty_detectable_all,true},
    {"sim.objectspecialproperty_pathplanning_ignored",sim_objectspecialproperty_pathplanning_ignored,true},
    // Model override properties:
    {"sim.modelproperty_not_collidable",sim_modelproperty_not_collidable,true},
    {"sim.modelproperty_not_measurable",sim_modelproperty_not_measurable,true},
    {"sim.modelproperty_not_renderable",sim_modelproperty_not_renderable,true},
    {"sim.modelproperty_not_detectable",sim_modelproperty_not_detectable,true},
    {"sim.modelproperty_not_dynamic",sim_modelproperty_not_dynamic,true},
    {"sim.modelproperty_not_respondable",sim_modelproperty_not_respondable,true},
    {"sim.modelproperty_not_reset",sim_modelproperty_not_reset,true},
    {"sim.modelproperty_not_visible",sim_modelproperty_not_visible,true},
    {"sim.modelproperty_scripts_inactive",sim_modelproperty_scripts_inactive,true},
    {"sim.modelproperty_not_showasinsidemodel",sim_modelproperty_not_showasinsidemodel,true},
    {"sim.modelproperty_not_model",sim_modelproperty_not_model,true},
    // Generic dialog styles:
    {"sim.dlgstyle_message",sim_dlgstyle_message,true},
    {"sim.dlgstyle_input",sim_dlgstyle_input,true},
    {"sim.dlgstyle_ok",sim_dlgstyle_ok,true},
    {"sim.dlgstyle_ok_cancel",sim_dlgstyle_ok_cancel,true},
    {"sim.dlgstyle_yes_no",sim_dlgstyle_yes_no,true},
    {"sim.dlgstyle_dont_center",sim_dlgstyle_dont_center,true},
    // Generic dialog return value:
    {"sim.dlgret_still_open",sim_dlgret_still_open,true},
    {"sim.dlgret_ok",sim_dlgret_ok,true},
    {"sim.dlgret_cancel",sim_dlgret_cancel,true},
    {"sim.dlgret_yes",sim_dlgret_yes,true},
    {"sim.dlgret_no",sim_dlgret_no,true},
    // Path properties:
    {"sim.pathproperty_show_line",sim_pathproperty_show_line,true},
    {"sim.pathproperty_show_orientation",sim_pathproperty_show_orientation,true},
    {"sim.pathproperty_closed_path",sim_pathproperty_closed_path,true},
    {"sim.pathproperty_automatic_orientation",sim_pathproperty_automatic_orientation,true},
    {"sim.pathproperty_flat_path",sim_pathproperty_flat_path,true},
    {"sim.pathproperty_show_position",sim_pathproperty_show_position,true},
    {"sim.pathproperty_keep_x_up",sim_pathproperty_keep_x_up,true},
    // distance calculation methods:
    {"sim.distcalcmethod_dl",sim_distcalcmethod_dl,true},
    {"sim.distcalcmethod_dac",sim_distcalcmethod_dac,true},
    {"sim.distcalcmethod_max_dl_dac",sim_distcalcmethod_max_dl_dac,true},
    {"sim.distcalcmethod_dl_and_dac",sim_distcalcmethod_dl_and_dac,true},
    {"sim.distcalcmethod_sqrt_dl2_and_dac2",sim_distcalcmethod_sqrt_dl2_and_dac2,true},
    {"sim.distcalcmethod_dl_if_nonzero",sim_distcalcmethod_dl_if_nonzero,true},
    {"sim.distcalcmethod_dac_if_nonzero",sim_distcalcmethod_dac_if_nonzero,true},
    // Boolean parameters:
    {"sim.boolparam_hierarchy_visible",sim_boolparam_hierarchy_visible,true},
    {"sim.boolparam_console_visible",sim_boolparam_console_visible,true},
    {"sim.boolparam_collision_handling_enabled",sim_boolparam_collision_handling_enabled,true},
    {"sim.boolparam_distance_handling_enabled",sim_boolparam_distance_handling_enabled,true},
    {"sim.boolparam_dynamics_handling_enabled",sim_boolparam_dynamics_handling_enabled,true},
    {"sim.boolparam_proximity_sensor_handling_enabled",sim_boolparam_proximity_sensor_handling_enabled,true},
    {"sim.boolparam_vision_sensor_handling_enabled",sim_boolparam_vision_sensor_handling_enabled,true},
    {"sim.boolparam_rendering_sensor_handling_enabled",sim_boolparam_vision_sensor_handling_enabled,true},
    {"sim.boolparam_mill_handling_enabled",sim_boolparam_mill_handling_enabled,true},
    {"sim.boolparam_browser_visible",sim_boolparam_browser_visible,true},
    {"sim.boolparam_scene_and_model_load_messages",sim_boolparam_scene_and_model_load_messages,true},
    {"sim.boolparam_shape_textures_are_visible",sim_boolparam_shape_textures_are_visible,true},
    {"sim.boolparam_display_enabled",sim_boolparam_display_enabled,true},
    {"sim.boolparam_infotext_visible",sim_boolparam_infotext_visible,true},
    {"sim.boolparam_statustext_open",sim_boolparam_statustext_open,true},
    {"sim.boolparam_fog_enabled",sim_boolparam_fog_enabled,true},
    {"sim.boolparam_rml2_available",sim_boolparam_rml2_available,true},
    {"sim.boolparam_rml4_available",sim_boolparam_rml4_available,true},
    {"sim.boolparam_mirrors_enabled",sim_boolparam_mirrors_enabled,true},
    {"sim.boolparam_aux_clip_planes_enabled",sim_boolparam_aux_clip_planes_enabled,true},
    {"sim.boolparam_full_model_copy_from_api",sim_boolparam_reserved3,true},
    {"sim.boolparam_realtime_simulation",sim_boolparam_realtime_simulation,true},
    {"sim.boolparam_online_mode",sim_boolparam_online_mode,true},
    {"sim.boolparam_br_partrepository",sim_boolparam_xr_partrepository,true},
    {"sim.boolparam_br_palletrepository",sim_boolparam_xr_palletrepository,true},
    {"sim.boolparam_br_jobfunc",sim_boolparam_xr_jobfunc,true},
    {"sim.boolparam_xr_partrepository",sim_boolparam_xr_partrepository,true},
    {"sim.boolparam_xr_palletrepository",sim_boolparam_xr_palletrepository,true},
    {"sim.boolparam_xr_jobfunc",sim_boolparam_xr_jobfunc,true},
    {"sim.boolparam_scene_closing",sim_boolparam_scene_closing,true},
    {"sim.boolparam_use_glfinish_cmd",sim_boolparam_use_glfinish_cmd,true},
    {"sim.boolparam_force_show_wireless_emission",sim_boolparam_force_show_wireless_emission,true},
    {"sim.boolparam_force_show_wireless_reception",sim_boolparam_force_show_wireless_reception,true},
    {"sim.boolparam_video_recording_triggered",sim_boolparam_video_recording_triggered,true},
    {"sim.boolparam_fullscreen",sim_boolparam_fullscreen,true},
    {"sim.boolparam_headless",sim_boolparam_headless,true},
    {"sim.boolparam_hierarchy_toolbarbutton_enabled",sim_boolparam_hierarchy_toolbarbutton_enabled,true},
    {"sim.boolparam_browser_toolbarbutton_enabled",sim_boolparam_browser_toolbarbutton_enabled,true},
    {"sim.boolparam_objectshift_toolbarbutton_enabled",sim_boolparam_objectshift_toolbarbutton_enabled,true},
    {"sim.boolparam_objectrotate_toolbarbutton_enabled",sim_boolparam_objectrotate_toolbarbutton_enabled,true},
    {"sim.boolparam_force_calcstruct_all_visible",sim_boolparam_force_calcstruct_all_visible,true},
    {"sim.boolparam_force_calcstruct_all",sim_boolparam_force_calcstruct_all,true},
    {"sim.boolparam_exit_request",sim_boolparam_exit_request,true},
    {"sim.boolparam_play_toolbarbutton_enabled",sim_boolparam_play_toolbarbutton_enabled,true},
    {"sim.boolparam_pause_toolbarbutton_enabled",sim_boolparam_pause_toolbarbutton_enabled,true},
    {"sim.boolparam_stop_toolbarbutton_enabled",sim_boolparam_stop_toolbarbutton_enabled,true},
    {"sim.boolparam_waiting_for_trigger",sim_boolparam_waiting_for_trigger,true},
    {"sim.boolparam_objproperties_toolbarbutton_enabled",sim_boolparam_objproperties_toolbarbutton_enabled,true},
    {"sim.boolparam_calcmodules_toolbarbutton_enabled",sim_boolparam_calcmodules_toolbarbutton_enabled,true},
    {"sim.boolparam_rosinterface_donotrunmainscript",sim_boolparam_rosinterface_donotrunmainscript,true},
    // Integer parameters:
    {"sim.intparam_error_report_mode",sim_intparam_error_report_mode,true},
    {"sim.intparam_program_version",sim_intparam_program_version,true},
    {"sim.intparam_program_full_version",sim_intparam_program_full_version,true},
    {"sim.intparam_verbosity",sim_intparam_verbosity,true},
    {"sim.intparam_statusbarverbosity",sim_intparam_statusbarverbosity,true},
    {"sim.intparam_dlgverbosity",sim_intparam_dlgverbosity,true},
    {"sim.intparam_exitcode",sim_intparam_exitcode,true},
    {"sim.intparam_videoencoderindex",sim_intparam_videoencoder_index,true},
    {"sim.intparam_compilation_version",sim_intparam_compilation_version,true},
    {"sim.intparam_current_page",sim_intparam_current_page,true},
    {"sim.intparam_flymode_camera_handle",sim_intparam_flymode_camera_handle,true},
    {"sim.intparam_dynamic_step_divider",sim_intparam_dynamic_step_divider,true},
    {"sim.intparam_dynamic_engine",sim_intparam_dynamic_engine,true},
    {"sim.intparam_server_port_start",sim_intparam_server_port_start,true},
    {"sim.intparam_server_port_range",sim_intparam_server_port_range,true},
    {"sim.intparam_server_port_next",sim_intparam_server_port_next,true},
    {"sim.intparam_visible_layers",sim_intparam_visible_layers,true},
    {"sim.intparam_infotext_style",sim_intparam_infotext_style,true},
    {"sim.intparam_settings",sim_intparam_settings,true},
    {"sim.intparam_qt_version",sim_intparam_qt_version,true},
    {"sim.intparam_platform",sim_intparam_platform,true},
    {"sim.intparam_scene_unique_id",sim_intparam_scene_unique_id,true},
    {"sim.intparam_edit_mode_type",sim_intparam_edit_mode_type,true},
    {"sim.intparam_work_thread_count",sim_intparam_work_thread_count,true}, // deprecated
    {"sim.intparam_mouse_x",sim_intparam_mouse_x,true},
    {"sim.intparam_mouse_y",sim_intparam_mouse_y,true},
    {"sim.intparam_core_count",sim_intparam_core_count,true},
    {"sim.intparam_work_thread_calc_time_ms",sim_intparam_work_thread_calc_time_ms,true}, // deprecated
    {"sim.intparam_idle_fps",sim_intparam_idle_fps,true},
    {"sim.intparam_prox_sensor_select_down",sim_intparam_prox_sensor_select_down,true},
    {"sim.intparam_prox_sensor_select_up",sim_intparam_prox_sensor_select_up,true},
    {"sim.intparam_stop_request_counter",sim_intparam_stop_request_counter,true},
    {"sim.intparam_program_revision",sim_intparam_program_revision,true},
    {"sim.intparam_mouse_buttons",sim_intparam_mouse_buttons,true},
    {"sim.intparam_dynamic_warning_disabled_mask",sim_intparam_dynamic_warning_disabled_mask,true},
    {"sim.intparam_simulation_warning_disabled_mask",sim_intparam_simulation_warning_disabled_mask,true},
    {"sim.intparam_scene_index",sim_intparam_scene_index,true},
    {"sim.intparam_motionplanning_seed",sim_intparam_motionplanning_seed,true},
    {"sim.intparam_speedmodifier",sim_intparam_speedmodifier,true},
    {"sim.intparam_dynamic_iteration_count",sim_intparam_dynamic_iteration_count,true},
    {"sim.intparam_job_count",sim_intparam_job_count,true},
    // Float parameters:
    {"sim.floatparam_rand",sim_floatparam_rand,true},
    {"sim.floatparam_simulation_time_step",sim_floatparam_simulation_time_step,true},
    {"sim.floatparam_stereo_distance",sim_floatparam_stereo_distance,true},
    {"sim.floatparam_dynamic_step_size",sim_floatparam_dynamic_step_size,true},
    {"sim.floatparam_mouse_wheel_zoom_factor",sim_floatparam_mouse_wheel_zoom_factor,true},
    // Array parameters:
    {"sim.arrayparam_gravity",sim_arrayparam_gravity,true},
    {"sim.arrayparam_fog",sim_arrayparam_fog,true},
    {"sim.arrayparam_fog_color",sim_arrayparam_fog_color,true},
    {"sim.arrayparam_background_color1",sim_arrayparam_background_color1,true},
    {"sim.arrayparam_background_color2",sim_arrayparam_background_color2,true},
    {"sim.arrayparam_ambient_light",sim_arrayparam_ambient_light,true},
    {"sim.arrayparam_random_euler",sim_arrayparam_random_euler,true},
    // String parameters:
    {"sim.stringparam_application_path",sim_stringparam_application_path,true},
    {"sim.stringparam_video_filename",sim_stringparam_video_filename,true},
    {"sim.stringparam_app_arg1",sim_stringparam_app_arg1,true},
    {"sim.stringparam_app_arg2",sim_stringparam_app_arg2,true},
    {"sim.stringparam_app_arg3",sim_stringparam_app_arg3,true},
    {"sim.stringparam_app_arg4",sim_stringparam_app_arg4,true},
    {"sim.stringparam_app_arg5",sim_stringparam_app_arg5,true},
    {"sim.stringparam_app_arg6",sim_stringparam_app_arg6,true},
    {"sim.stringparam_app_arg7",sim_stringparam_app_arg7,true},
    {"sim.stringparam_app_arg8",sim_stringparam_app_arg8,true},
    {"sim.stringparam_app_arg9",sim_stringparam_app_arg9,true},
    {"sim.stringparam_scene_path_and_name",sim_stringparam_scene_path_and_name,true},
    {"sim.stringparam_remoteapi_temp_file_dir",sim_stringparam_remoteapi_temp_file_dir,true},
    {"sim.stringparam_scene_path",sim_stringparam_scene_path,true},
    {"sim.stringparam_scene_name",sim_stringparam_scene_name,true},
    {"sim.stringparam_job",sim_stringparam_job,true},
    {"sim.stringparam_job0",sim_stringparam_job0,true},
    {"sim.stringparam_scene_unique_id",sim_stringparam_scene_unique_id,true},
    {"sim.stringparam_machine_id",sim_stringparam_machine_id,true},
    {"sim.stringparam_machine_id_legacy",sim_stringparam_machine_id_legacy,true},
    {"sim.stringparam_verbosity",sim_stringparam_verbosity,true},
    {"sim.stringparam_statusbarverbosity",sim_stringparam_statusbarverbosity,true},
    {"sim.stringparam_dlgverbosity",sim_stringparam_dlgverbosity,true},
    {"sim.stringparam_logfilter",sim_stringparam_consolelogfilter,true},

    // verbosity:
    {"sim.verbosity_useglobal",sim_verbosity_useglobal,true},
    {"sim.verbosity_none",sim_verbosity_none,true},
    {"sim.verbosity_errors",sim_verbosity_errors,true},
    {"sim.verbosity_warnings",sim_verbosity_warnings,true},
    {"sim.verbosity_loadinfos",sim_verbosity_loadinfos,true},
    {"sim.verbosity_questions",sim_verbosity_questions,true},
    {"sim.verbosity_scripterrors",sim_verbosity_scripterrors,true},
    {"sim.verbosity_scriptwarnings",sim_verbosity_scriptwarnings,true},
    {"sim.verbosity_scriptinfos",sim_verbosity_scriptinfos,true},
    {"sim.verbosity_msgs",sim_verbosity_msgs,true},
    {"sim.verbosity_infos",sim_verbosity_infos,true},
    {"sim.verbosity_debug",sim_verbosity_debug,true},
    {"sim.verbosity_trace",sim_verbosity_trace,true},
    {"sim.verbosity_tracelua",sim_verbosity_tracelua,true},
    {"sim.verbosity_traceall",sim_verbosity_traceall,true},
    {"sim.verbosity_default",sim_verbosity_default,true},
    {"sim.verbosity_undecorated",sim_verbosity_undecorated,true},

    // module info:
    {"sim.moduleinfo_extversionstr",sim_moduleinfo_extversionstr,true},
    {"sim.moduleinfo_builddatestr",sim_moduleinfo_builddatestr,true},
    {"sim.moduleinfo_extversionint",sim_moduleinfo_extversionint,true},
    {"sim.moduleinfo_verbosity",sim_moduleinfo_verbosity,true},
    {"sim.moduleinfo_statusbarverbosity",sim_moduleinfo_statusbarverbosity,true},

    // Rendering attributes:
    {"sim.displayattribute_renderpass",sim_displayattribute_renderpass,true},
    {"sim.displayattribute_depthpass",sim_displayattribute_depthpass,true},
    {"sim.displayattribute_pickpass",sim_displayattribute_pickpass,true},
    {"sim.displayattribute_selected",sim_displayattribute_selected,true},
    {"sim.displayattribute_mainselection",sim_displayattribute_mainselection,true},
    {"sim.displayattribute_forcewireframe",sim_displayattribute_forcewireframe,true},
    {"sim.displayattribute_forbidwireframe",sim_displayattribute_forbidwireframe,true},
    {"sim.displayattribute_forbidedges",sim_displayattribute_forbidedges,true},
    {"sim.displayattribute_originalcolors",sim_displayattribute_originalcolors,true},
    {"sim.displayattribute_ignorelayer",sim_displayattribute_ignorelayer,true},
    {"sim.displayattribute_forvisionsensor",sim_displayattribute_forvisionsensor,true},
    {"sim.displayattribute_colorcodedpickpass",sim_displayattribute_colorcodedpickpass,true},
    {"sim.displayattribute_colorcoded",sim_displayattribute_colorcoded,true},
    {"sim.displayattribute_trianglewireframe",sim_displayattribute_trianglewireframe,true},
    {"sim.displayattribute_thickEdges",sim_displayattribute_thickEdges,true},
    {"sim.displayattribute_dynamiccontentonly",sim_displayattribute_dynamiccontentonly,true},
    {"sim.displayattribute_mirror",sim_displayattribute_mirror,true},
    {"sim.displayattribute_useauxcomponent",sim_displayattribute_useauxcomponent,true},
    {"sim.displayattribute_ignorerenderableflag",sim_displayattribute_ignorerenderableflag,true},
    {"sim.displayattribute_noopenglcallbacks",sim_displayattribute_noopenglcallbacks,true},
    {"sim.displayattribute_noghosts",sim_displayattribute_noghosts,true},
    {"sim.displayattribute_nopointclouds",sim_displayattribute_nopointclouds,true},
    {"sim.displayattribute_nodrawingobjects",sim_displayattribute_nodrawingobjects,true},
    {"sim.displayattribute_noparticles",sim_displayattribute_noparticles,true},
    {"sim.displayattribute_colorcodedtriangles",sim_displayattribute_colorcodedtriangles,true},
    // Navigation and selection modes with the mouse:
    {"sim.navigation_passive",sim_navigation_passive,true},
    {"sim.navigation_camerashift",sim_navigation_camerashift,true},
    {"sim.navigation_camerarotate",sim_navigation_camerarotate,true},
    {"sim.navigation_camerazoom",sim_navigation_camerazoom,true},
    {"sim.navigation_cameratilt",sim_navigation_cameratilt,true},
    {"sim.navigation_cameraangle",sim_navigation_cameraangle,true},
    {"sim.navigation_objectshift",sim_navigation_objectshift,true},
    {"sim.navigation_objectrotate",sim_navigation_objectrotate,true},
    {"sim.navigation_createpathpoint",sim_navigation_createpathpoint,true},
    {"sim.navigation_clickselection",sim_navigation_clickselection,true},
    {"sim.navigation_ctrlselection",sim_navigation_ctrlselection,true},
    {"sim.navigation_shiftselection",sim_navigation_shiftselection,true},
    {"sim.navigation_camerazoomwheel",sim_navigation_camerazoomwheel,true},
    {"sim.navigation_camerarotaterightbutton",sim_navigation_camerarotaterightbutton,true},
    {"sim.navigation_camerarotatemiddlebutton",sim_navigation_camerarotatemiddlebutton,true},
    // drawing objects
    {"sim.drawing_points",sim_drawing_points,true},
    {"sim.drawing_lines",sim_drawing_lines,true},
    {"sim.drawing_triangles",sim_drawing_triangles,true},
    {"sim.drawing_trianglepoints",sim_drawing_trianglepoints,true},
    {"sim.drawing_quadpoints",sim_drawing_quadpoints,true},
    {"sim.drawing_discpoints",sim_drawing_discpoints,true},
    {"sim.drawing_cubepoints",sim_drawing_cubepoints,true},
    {"sim.drawing_spherepoints",sim_drawing_spherepoints,true},
    {"sim.drawing_itemcolors",sim_drawing_itemcolors,true},
    {"sim.drawing_vertexcolors",sim_drawing_vertexcolors,true},
    {"sim.drawing_itemsizes",sim_drawing_itemsizes,true},
    {"sim.drawing_backfaceculling",sim_drawing_backfaceculling,true},
    {"sim.drawing_wireframe",sim_drawing_wireframe,true},
    {"sim.drawing_painttag",sim_drawing_painttag,true},
    {"sim.drawing_followparentvisibility",sim_drawing_followparentvisibility,true},
    {"sim.drawing_cyclic",sim_drawing_cyclic,true},
    {"sim.drawing_50percenttransparency",sim_drawing_50percenttransparency,true},
    {"sim.drawing_25percenttransparency",sim_drawing_25percenttransparency,true},
    {"sim.drawing_12percenttransparency",sim_drawing_12percenttransparency,true},
    {"sim.drawing_emissioncolor",sim_drawing_emissioncolor,true},
    {"sim.drawing_facingcamera",sim_drawing_facingcamera,true},
    {"sim.drawing_overlay",sim_drawing_overlay,true},
    {"sim.drawing_itemtransparency",sim_drawing_itemtransparency,true},
    {"sim.drawing_persistent",sim_drawing_persistent,true},
    {"sim.drawing_auxchannelcolor1",sim_drawing_auxchannelcolor1,true},
    {"sim.drawing_auxchannelcolor2",sim_drawing_auxchannelcolor2,true},
    // Banners
    {"sim.banner_left",sim_banner_left,true},
    {"sim.banner_right",sim_banner_right,true},
    {"sim.banner_nobackground",sim_banner_nobackground,true},
    {"sim.banner_overlay",sim_banner_overlay,true},
    {"sim.banner_followparentvisibility",sim_banner_followparentvisibility,true},
    {"sim.banner_clickselectsparent",sim_banner_clickselectsparent,true},
    {"sim.banner_clicktriggersevent",sim_banner_clicktriggersevent,true},
    {"sim.banner_facingcamera",sim_banner_facingcamera,true},
    {"sim.banner_fullyfacingcamera",sim_banner_fullyfacingcamera,true},
    {"sim.banner_backfaceculling",sim_banner_backfaceculling,true},
    {"sim.banner_keepsamesize",sim_banner_keepsamesize,true},
    {"sim.banner_bitmapfont",sim_banner_bitmapfont,true},
    // particle objects
    {"sim.particle_points1",sim_particle_points1,true},
    {"sim.particle_points2",sim_particle_points2,true},
    {"sim.particle_points4",sim_particle_points4,true},
    {"sim.particle_roughspheres",sim_particle_roughspheres,true},
    {"sim.particle_spheres",sim_particle_spheres,true},
    {"sim.particle_respondable1to4",sim_particle_respondable1to4,true},
    {"sim.particle_respondable5to8",sim_particle_respondable5to8,true},
    {"sim.particle_particlerespondable",sim_particle_particlerespondable,true},
    {"sim.particle_ignoresgravity",sim_particle_ignoresgravity,true},
    {"sim.particle_invisible",sim_particle_invisible,true},
    {"sim.particle_painttag",sim_particle_painttag,true},
    {"sim.particle_itemsizes",sim_particle_itemsizes,true},
    {"sim.particle_itemdensities",sim_particle_itemdensities,true},
    {"sim.particle_itemcolors",sim_particle_itemcolors,true},
    {"sim.particle_cyclic",sim_particle_cyclic,true},
    {"sim.particle_emissioncolor",sim_particle_emissioncolor,true},
    {"sim.particle_water",sim_particle_water,true},
    // joint modes
    {"sim.jointmode_passive",sim_jointmode_passive,true},
    {"sim.jointmode_dependent",sim_jointmode_dependent,true},
    {"sim.jointmode_force",sim_jointmode_force,true},
    // file dialog styles
    {"sim.filedlg_type_load",sim_filedlg_type_load,true},
    {"sim.filedlg_type_save",sim_filedlg_type_save,true},
    {"sim.filedlg_type_load_multiple",sim_filedlg_type_load_multiple,true},
    {"sim.filedlg_type_folder",sim_filedlg_type_folder,true},
    // message box styles
    {"sim.msgbox_type_info",sim_msgbox_type_info,true},
    {"sim.msgbox_type_question",sim_msgbox_type_question,true},
    {"sim.msgbox_type_warning",sim_msgbox_type_warning,true},
    {"sim.msgbox_type_critical",sim_msgbox_type_critical,true},
    // message box buttons
    {"sim.msgbox_buttons_ok",sim_msgbox_buttons_ok,true},
    {"sim.msgbox_buttons_yesno",sim_msgbox_buttons_yesno,true},
    {"sim.msgbox_buttons_yesnocancel",sim_msgbox_buttons_yesnocancel,true},
    {"sim.msgbox_buttons_okcancel",sim_msgbox_buttons_okcancel,true},
    // message box return values
    {"sim.msgbox_return_cancel",sim_msgbox_return_cancel,true},
    {"sim.msgbox_return_no",sim_msgbox_return_no,true},
    {"sim.msgbox_return_yes",sim_msgbox_return_yes,true},
    {"sim.msgbox_return_ok",sim_msgbox_return_ok,true},
    {"sim.msgbox_return_error",sim_msgbox_return_error,true},
    // physics engine
    {"sim.physics_bullet",sim_physics_bullet,true},
    {"sim.physics_ode",sim_physics_ode,true},
    {"sim.physics_vortex",sim_physics_vortex,true},
    {"sim.physics_newton",sim_physics_newton,true},
    // pure primitives type
    {"sim.pure_primitive_none",sim_pure_primitive_none,true},
    {"sim.pure_primitive_plane",sim_pure_primitive_plane,true},
    {"sim.pure_primitive_disc",sim_pure_primitive_disc,true},
    {"sim.pure_primitive_cuboid",sim_pure_primitive_cuboid,true},
    {"sim.pure_primitive_spheroid",sim_pure_primitive_spheroid,true},
    {"sim.pure_primitive_cylinder",sim_pure_primitive_cylinder,true},
    {"sim.pure_primitive_cone",sim_pure_primitive_cone,true},
    {"sim.pure_primitive_heightfield",sim_pure_primitive_heightfield,true},
    // dummy-dummy link types
    {"sim.dummy_linktype_dynamics_loop_closure",sim_dummy_linktype_dynamics_loop_closure,true},
    // color components
    {"sim.colorcomponent_ambient",sim_colorcomponent_ambient,true},
    {"sim.colorcomponent_ambient_diffuse",sim_colorcomponent_ambient_diffuse,true},
    {"sim.colorcomponent_diffuse",sim_colorcomponent_diffuse,true},
    {"sim.colorcomponent_specular",sim_colorcomponent_specular,true},
    {"sim.colorcomponent_emission",sim_colorcomponent_emission,true},
    {"sim.colorcomponent_transparency",sim_colorcomponent_transparency,true},
    {"sim.colorcomponent_auxiliary",sim_colorcomponent_auxiliary,true},
    // volume types
    {"sim.volume_ray",sim_volume_ray,true},
    {"sim.volume_randomizedray",sim_volume_randomizedray,true},
    {"sim.volume_pyramid",sim_volume_pyramid,true},
    {"sim.volume_cylinder",sim_volume_cylinder,true},
    {"sim.volume_disc",sim_volume_disc,true},
    {"sim.volume_cone",sim_volume_cone,true},
    // Object int/float/string parameters
    // scene objects
    {"sim.objintparam_visibility_layer",sim_objintparam_visibility_layer,true},
    {"sim.objfloatparam_abs_x_velocity",sim_objfloatparam_abs_x_velocity,true},
    {"sim.objfloatparam_abs_y_velocity",sim_objfloatparam_abs_y_velocity,true},
    {"sim.objfloatparam_abs_z_velocity",sim_objfloatparam_abs_z_velocity,true},
    {"sim.objfloatparam_abs_rot_velocity",sim_objfloatparam_abs_rot_velocity,true},
    {"sim.objfloatparam_objbbox_min_x",sim_objfloatparam_objbbox_min_x,true},
    {"sim.objfloatparam_objbbox_min_y",sim_objfloatparam_objbbox_min_y,true},
    {"sim.objfloatparam_objbbox_min_z",sim_objfloatparam_objbbox_min_z,true},
    {"sim.objfloatparam_objbbox_max_x",sim_objfloatparam_objbbox_max_x,true},
    {"sim.objfloatparam_objbbox_max_y",sim_objfloatparam_objbbox_max_y,true},
    {"sim.objfloatparam_objbbox_max_z",sim_objfloatparam_objbbox_max_z,true},
    {"sim.objfloatparam_modelbbox_min_x",sim_objfloatparam_modelbbox_min_x,true},
    {"sim.objfloatparam_modelbbox_min_y",sim_objfloatparam_modelbbox_min_y,true},
    {"sim.objfloatparam_modelbbox_min_z",sim_objfloatparam_modelbbox_min_z,true},
    {"sim.objfloatparam_modelbbox_max_x",sim_objfloatparam_modelbbox_max_x,true},
    {"sim.objfloatparam_modelbbox_max_y",sim_objfloatparam_modelbbox_max_y,true},
    {"sim.objfloatparam_modelbbox_max_z",sim_objfloatparam_modelbbox_max_z,true},
    {"sim.objintparam_collection_self_collision_indicator",sim_objintparam_collection_self_collision_indicator,true},
    {"sim.objfloatparam_transparency_offset",sim_objfloatparam_transparency_offset,true},
    {"sim.objfloatparam_size_factor",sim_objfloatparam_size_factor,true},
    {"sim.objintparam_child_role",sim_objintparam_child_role,true},
    {"sim.objintparam_parent_role",sim_objintparam_parent_role,true},
    {"sim.objintparam_manipulation_permissions",sim_objintparam_manipulation_permissions,true},
    {"sim.objintparam_illumination_handle",sim_objintparam_illumination_handle,true},
    {"sim.objstringparam_dna",sim_objstringparam_dna,true},
    {"sim.objstringparam_unique_id",sim_objstringparam_unique_id,true},
    {"sim.objintparam_visible",sim_objintparam_visible,true},

    // vision_sensors
    {"sim.visionfloatparam_near_clipping",sim_visionfloatparam_near_clipping,true},
    {"sim.visionfloatparam_far_clipping",sim_visionfloatparam_far_clipping,true},
    {"sim.visionintparam_resolution_x",sim_visionintparam_resolution_x,true},
    {"sim.visionintparam_resolution_y",sim_visionintparam_resolution_y,true},
    {"sim.visionfloatparam_perspective_angle",sim_visionfloatparam_perspective_angle,true},
    {"sim.visionfloatparam_ortho_size",sim_visionfloatparam_ortho_size,true},
    {"sim.visionintparam_disabled_light_components",sim_visionintparam_disabled_light_components,true},
    {"sim.visionintparam_rendering_attributes",sim_visionintparam_rendering_attributes,true},
    {"sim.visionintparam_entity_to_render",sim_visionintparam_entity_to_render,true},
    {"sim.visionintparam_windowed_size_x",sim_visionintparam_windowed_size_x,true},
    {"sim.visionintparam_windowed_size_y",sim_visionintparam_windowed_size_y,true},
    {"sim.visionintparam_windowed_pos_x",sim_visionintparam_windowed_pos_x,true},
    {"sim.visionintparam_windowed_pos_y",sim_visionintparam_windowed_pos_y,true},
    {"sim.visionintparam_pov_focal_blur",sim_visionintparam_pov_focal_blur,true},
    {"sim.visionfloatparam_pov_blur_distance",sim_visionfloatparam_pov_blur_distance,true},
    {"sim.visionfloatparam_pov_aperture",sim_visionfloatparam_pov_aperture,true},
    {"sim.visionintparam_pov_blur_sampled",sim_visionintparam_pov_blur_sampled,true},
    {"sim.visionintparam_render_mode",sim_visionintparam_render_mode,true},
    {"sim.visionintparam_perspective_operation",sim_visionintparam_perspective_operation,true},
    // joints
    {"sim.jointintparam_motor_enabled",sim_jointintparam_motor_enabled,true},
    {"sim.jointintparam_ctrl_enabled",sim_jointintparam_ctrl_enabled,true},
    {"sim.jointfloatparam_pid_p",sim_jointfloatparam_pid_p,true},
    {"sim.jointfloatparam_pid_i",sim_jointfloatparam_pid_i,true},
    {"sim.jointfloatparam_pid_d",sim_jointfloatparam_pid_d,true},
    {"sim.jointfloatparam_intrinsic_x",sim_jointfloatparam_intrinsic_x,true},
    {"sim.jointfloatparam_intrinsic_y",sim_jointfloatparam_intrinsic_y,true},
    {"sim.jointfloatparam_intrinsic_z",sim_jointfloatparam_intrinsic_z,true},
    {"sim.jointfloatparam_intrinsic_qx",sim_jointfloatparam_intrinsic_qx,true},
    {"sim.jointfloatparam_intrinsic_qy",sim_jointfloatparam_intrinsic_qy,true},
    {"sim.jointfloatparam_intrinsic_qz",sim_jointfloatparam_intrinsic_qz,true},
    {"sim.jointfloatparam_intrinsic_qw",sim_jointfloatparam_intrinsic_qw,true},
    {"sim.jointfloatparam_velocity",sim_jointfloatparam_velocity,true},
    {"sim.jointfloatparam_spherical_qx",sim_jointfloatparam_spherical_qx,true},
    {"sim.jointfloatparam_spherical_qy",sim_jointfloatparam_spherical_qy,true},
    {"sim.jointfloatparam_spherical_qz",sim_jointfloatparam_spherical_qz,true},
    {"sim.jointfloatparam_spherical_qw",sim_jointfloatparam_spherical_qw,true},
    {"sim.jointfloatparam_upper_limit",sim_jointfloatparam_upper_limit,true},
    {"sim.jointfloatparam_kc_k",sim_jointfloatparam_kc_k,true},
    {"sim.jointfloatparam_kc_c",sim_jointfloatparam_kc_c,true},
    {"sim.jointfloatparam_error_x",sim_jointfloatparam_error_x,true},
    {"sim.jointfloatparam_error_y",sim_jointfloatparam_error_y,true},
    {"sim.jointfloatparam_error_z",sim_jointfloatparam_error_z,true},
    {"sim.jointfloatparam_error_a",sim_jointfloatparam_error_a,true},
    {"sim.jointfloatparam_error_b",sim_jointfloatparam_error_b,true},
    {"sim.jointfloatparam_error_g",sim_jointfloatparam_error_g,true},
    {"sim.jointfloatparam_error_pos",sim_jointfloatparam_error_pos,true},
    {"sim.jointfloatparam_error_angle",sim_jointfloatparam_error_angle,true},
    {"sim.jointintparam_velocity_lock",sim_jointintparam_velocity_lock,true},
    {"sim.jointintparam_vortex_dep_handle",sim_jointintparam_vortex_dep_handle,true},
    {"sim.jointfloatparam_vortex_dep_multiplication",sim_jointfloatparam_vortex_dep_multiplication,true},
    {"sim.jointfloatparam_vortex_dep_offset",sim_jointfloatparam_vortex_dep_offset,true},
    {"sim.jointfloatparam_screw_pitch",sim_jointfloatparam_screw_pitch,true},
    // shapes
    {"sim.shapefloatparam_init_velocity_x",sim_shapefloatparam_init_velocity_x,true},
    {"sim.shapefloatparam_init_velocity_y",sim_shapefloatparam_init_velocity_y,true},
    {"sim.shapefloatparam_init_velocity_z",sim_shapefloatparam_init_velocity_z,true},
    {"sim.shapeintparam_static",sim_shapeintparam_static,true},
    {"sim.shapeintparam_respondable",sim_shapeintparam_respondable,true},
    {"sim.shapefloatparam_mass",sim_shapefloatparam_mass,true},
    {"sim.shapefloatparam_texture_x",sim_shapefloatparam_texture_x,true},
    {"sim.shapefloatparam_texture_y",sim_shapefloatparam_texture_y,true},
    {"sim.shapefloatparam_texture_z",sim_shapefloatparam_texture_z,true},
    {"sim.shapefloatparam_texture_a",sim_shapefloatparam_texture_a,true},
    {"sim.shapefloatparam_texture_b",sim_shapefloatparam_texture_b,true},
    {"sim.shapefloatparam_texture_g",sim_shapefloatparam_texture_g,true},
    {"sim.shapefloatparam_texture_scaling_x",sim_shapefloatparam_texture_scaling_x,true},
    {"sim.shapefloatparam_texture_scaling_y",sim_shapefloatparam_texture_scaling_y,true},
    {"sim.shapeintparam_culling",sim_shapeintparam_culling,true},
    {"sim.shapeintparam_wireframe",sim_shapeintparam_wireframe,true},
    {"sim.shapeintparam_compound",sim_shapeintparam_compound,true},
    {"sim.shapeintparam_convex",sim_shapeintparam_convex,true},
    {"sim.shapeintparam_convex_check",sim_shapeintparam_convex_check,true},
    {"sim.shapeintparam_respondable_mask",sim_shapeintparam_respondable_mask,true},
    {"sim.shapefloatparam_init_ang_velocity_x",sim_shapefloatparam_init_ang_velocity_x,true},
    {"sim.shapefloatparam_init_ang_velocity_y",sim_shapefloatparam_init_ang_velocity_y,true},
    {"sim.shapefloatparam_init_ang_velocity_z",sim_shapefloatparam_init_ang_velocity_z,true},
    {"sim.shapefloatparam_init_velocity_a",sim_shapefloatparam_init_ang_velocity_x,false},
    {"sim.shapefloatparam_init_velocity_b",sim_shapefloatparam_init_ang_velocity_y,false},
    {"sim.shapefloatparam_init_velocity_g",sim_shapefloatparam_init_ang_velocity_z,false},
    {"sim.shapestringparam_color_name",sim_shapestringparam_color_name,true},
    {"sim.shapeintparam_edge_visibility",sim_shapeintparam_edge_visibility,true},
    {"sim.shapefloatparam_shading_angle",sim_shapefloatparam_shading_angle,true},
    {"sim.shapefloatparam_edge_angle",sim_shapefloatparam_edge_angle,true},
    {"sim.shapeintparam_edge_borders_hidden",sim_shapeintparam_edge_borders_hidden,true},
    // proximity sensors
    {"sim.proxintparam_ray_invisibility",sim_proxintparam_ray_invisibility,true},
    {"sim.proxintparam_volume_type",sim_proxintparam_volume_type,true},
    {"sim.proxintparam_entity_to_detect",sim_proxintparam_entity_to_detect,true},
    // proximity sensors
    {"sim.forcefloatparam_error_x",sim_forcefloatparam_error_x,true},
    {"sim.forcefloatparam_error_y",sim_forcefloatparam_error_y,true},
    {"sim.forcefloatparam_error_z",sim_forcefloatparam_error_z,true},
    {"sim.forcefloatparam_error_a",sim_forcefloatparam_error_a,true},
    {"sim.forcefloatparam_error_b",sim_forcefloatparam_error_b,true},
    {"sim.forcefloatparam_error_g",sim_forcefloatparam_error_g,true},
    {"sim.forcefloatparam_error_pos",sim_forcefloatparam_error_pos,true},
    {"sim.forcefloatparam_error_angle",sim_forcefloatparam_error_angle,true},
    // lights
    {"sim.lightintparam_pov_casts_shadows",sim_lightintparam_pov_casts_shadows,true},
    {"sim.lightfloatparam_spot_exponent",sim_lightfloatparam_spot_exponent,true},
    {"sim.lightfloatparam_spot_cutoff",sim_lightfloatparam_spot_cutoff,true},
    {"sim.lightfloatparam_const_attenuation",sim_lightfloatparam_const_attenuation,true},
    {"sim.lightfloatparam_lin_attenuation",sim_lightfloatparam_lin_attenuation,true},
    {"sim.lightfloatparam_quad_attenuation",sim_lightfloatparam_quad_attenuation,true},

    // cameras
    {"sim.camerafloatparam_near_clipping",sim_camerafloatparam_near_clipping,true},
    {"sim.camerafloatparam_far_clipping",sim_camerafloatparam_far_clipping,true},
    {"sim.cameraintparam_disabled_light_components",sim_cameraintparam_disabled_light_components,true},
    {"sim.camerafloatparam_perspective_angle",sim_camerafloatparam_perspective_angle,true},
    {"sim.camerafloatparam_ortho_size",sim_camerafloatparam_ortho_size,true},
    {"sim.cameraintparam_rendering_attributes",sim_cameraintparam_rendering_attributes,true},
    {"sim.cameraintparam_pov_focal_blur",sim_cameraintparam_pov_focal_blur,true},
    {"sim.camerafloatparam_pov_blur_distance",sim_camerafloatparam_pov_blur_distance,true},
    {"sim.camerafloatparam_pov_aperture",sim_camerafloatparam_pov_aperture,true},
    {"sim.cameraintparam_pov_blur_samples",sim_cameraintparam_pov_blur_samples,true},
    // dummies
    {"sim.dummyintparam_link_type",sim_dummyintparam_link_type,true},
    {"sim.dummyintparam_follow_path",sim_dummyintparam_follow_path,true},
    {"sim.dummyfloatparam_follow_path_offset",sim_dummyfloatparam_follow_path_offset,true},
    // mills
    {"sim.millintparam_volume_type",sim_millintparam_volume_type,true},
    // mirrors
    {"sim.mirrorfloatparam_width",sim_mirrorfloatparam_width,true},
    {"sim.mirrorfloatparam_height",sim_mirrorfloatparam_height,true},
    {"sim.mirrorfloatparam_reflectance",sim_mirrorfloatparam_reflectance,true},
    {"sim.mirrorintparam_enable",sim_mirrorintparam_enable,true},
    // Bullet engine params
    {"sim.bullet_global_stepsize",sim_bullet_global_stepsize,true},
    {"sim.bullet_global_internalscalingfactor",sim_bullet_global_internalscalingfactor,true},
    {"sim.bullet_global_collisionmarginfactor",sim_bullet_global_collisionmarginfactor,true},
    {"sim.bullet_global_constraintsolvingiterations",sim_bullet_global_constraintsolvingiterations,true},
    {"sim.bullet_global_bitcoded",sim_bullet_global_bitcoded,true},
    {"sim.bullet_global_constraintsolvertype",sim_bullet_global_constraintsolvertype,true},
    {"sim.bullet_global_fullinternalscaling",sim_bullet_global_fullinternalscaling,true},
    {"sim.bullet_joint_stoperp",sim_bullet_joint_stoperp,true},
    {"sim.bullet_joint_stopcfm",sim_bullet_joint_stopcfm,true},
    {"sim.bullet_joint_normalcfm",sim_bullet_joint_normalcfm,true},
    {"sim.bullet_body_restitution",sim_bullet_body_restitution,true},
    {"sim.bullet_body_oldfriction",sim_bullet_body_oldfriction,true},
    {"sim.bullet_body_friction",sim_bullet_body_friction,true},
    {"sim.bullet_body_lineardamping",sim_bullet_body_lineardamping,true},
    {"sim.bullet_body_angulardamping",sim_bullet_body_angulardamping,true},
    {"sim.bullet_body_nondefaultcollisionmargingfactor",sim_bullet_body_nondefaultcollisionmargingfactor,true},
    {"sim.bullet_body_nondefaultcollisionmargingfactorconvex",sim_bullet_body_nondefaultcollisionmargingfactorconvex,true},
    {"sim.bullet_body_bitcoded",sim_bullet_body_bitcoded,true},
    {"sim.bullet_body_sticky",sim_bullet_body_sticky,true},
    {"sim.bullet_body_usenondefaultcollisionmargin",sim_bullet_body_usenondefaultcollisionmargin,true},
    {"sim.bullet_body_usenondefaultcollisionmarginconvex",sim_bullet_body_usenondefaultcollisionmarginconvex,true},
    {"sim.bullet_body_autoshrinkconvex",sim_bullet_body_autoshrinkconvex,true},
    // Ode engine params
    {"sim.ode_global_stepsize",sim_ode_global_stepsize,true},
    {"sim.ode_global_internalscalingfactor",sim_ode_global_internalscalingfactor,true},
    {"sim.ode_global_cfm",sim_ode_global_cfm,true},
    {"sim.ode_global_erp",sim_ode_global_erp,true},
    {"sim.ode_global_constraintsolvingiterations",sim_ode_global_constraintsolvingiterations,true},
    {"sim.ode_global_bitcoded",sim_ode_global_bitcoded,true},
    {"sim.ode_global_randomseed",sim_ode_global_randomseed,true},
    {"sim.ode_global_fullinternalscaling",sim_ode_global_fullinternalscaling,true},
    {"sim.ode_global_quickstep",sim_ode_global_quickstep,true},
    {"sim.ode_joint_stoperp",sim_ode_joint_stoperp,true},
    {"sim.ode_joint_stopcfm",sim_ode_joint_stopcfm,true},
    {"sim.ode_joint_bounce",sim_ode_joint_bounce,true},
    {"sim.ode_joint_fudgefactor",sim_ode_joint_fudgefactor,true},
    {"sim.ode_joint_normalcfm",sim_ode_joint_normalcfm,true},
    {"sim.ode_body_friction",sim_ode_body_friction,true},
    {"sim.ode_body_softerp",sim_ode_body_softerp,true},
    {"sim.ode_body_softcfm",sim_ode_body_softcfm,true},
    {"sim.ode_body_lineardamping",sim_ode_body_lineardamping,true},
    {"sim.ode_body_angulardamping",sim_ode_body_angulardamping,true},
    {"sim.ode_body_maxcontacts",sim_ode_body_maxcontacts,true},
    // Vortex engine params
    {"sim.vortex_global_stepsize",sim_vortex_global_stepsize,true},
    {"sim.vortex_global_internalscalingfactor",sim_vortex_global_internalscalingfactor,true},
    {"sim.vortex_global_contacttolerance",sim_vortex_global_contacttolerance,true},
    {"sim.vortex_global_constraintlinearcompliance",sim_vortex_global_constraintlinearcompliance,true},
    {"sim.vortex_global_constraintlineardamping",sim_vortex_global_constraintlineardamping,true},
    {"sim.vortex_global_constraintlinearkineticloss",sim_vortex_global_constraintlinearkineticloss,true},
    {"sim.vortex_global_constraintangularcompliance",sim_vortex_global_constraintangularcompliance,true},
    {"sim.vortex_global_constraintangulardamping",sim_vortex_global_constraintangulardamping,true},
    {"sim.vortex_global_constraintangularkineticloss",sim_vortex_global_constraintangularkineticloss,true},
    {"sim.vortex_global_bitcoded",sim_vortex_global_bitcoded,true},
    {"sim.vortex_global_autosleep",sim_vortex_global_autosleep,true},
    {"sim.vortex_global_multithreading",sim_vortex_global_multithreading,true},
    {"sim.vortex_joint_lowerlimitdamping",sim_vortex_joint_lowerlimitdamping,true},
    {"sim.vortex_joint_upperlimitdamping",sim_vortex_joint_upperlimitdamping,true},
    {"sim.vortex_joint_lowerlimitstiffness",sim_vortex_joint_lowerlimitstiffness,true},
    {"sim.vortex_joint_upperlimitstiffness",sim_vortex_joint_upperlimitstiffness,true},
    {"sim.vortex_joint_lowerlimitrestitution",sim_vortex_joint_lowerlimitrestitution,true},
    {"sim.vortex_joint_upperlimitrestitution",sim_vortex_joint_upperlimitrestitution,true},
    {"sim.vortex_joint_lowerlimitmaxforce",sim_vortex_joint_lowerlimitmaxforce,true},
    {"sim.vortex_joint_upperlimitmaxforce",sim_vortex_joint_upperlimitmaxforce,true},
    {"sim.vortex_joint_motorconstraintfrictioncoeff",sim_vortex_joint_motorconstraintfrictioncoeff,true},
    {"sim.vortex_joint_motorconstraintfrictionmaxforce",sim_vortex_joint_motorconstraintfrictionmaxforce,true},
    {"sim.vortex_joint_motorconstraintfrictionloss",sim_vortex_joint_motorconstraintfrictionloss,true},
    {"sim.vortex_joint_p0loss",sim_vortex_joint_p0loss,true},
    {"sim.vortex_joint_p0stiffness",sim_vortex_joint_p0stiffness,true},
    {"sim.vortex_joint_p0damping",sim_vortex_joint_p0damping,true},
    {"sim.vortex_joint_p0frictioncoeff",sim_vortex_joint_p0frictioncoeff,true},
    {"sim.vortex_joint_p0frictionmaxforce",sim_vortex_joint_p0frictionmaxforce,true},
    {"sim.vortex_joint_p0frictionloss",sim_vortex_joint_p0frictionloss,true},
    {"sim.vortex_joint_p1loss",sim_vortex_joint_p1loss,true},
    {"sim.vortex_joint_p1stiffness",sim_vortex_joint_p1stiffness,true},
    {"sim.vortex_joint_p1damping",sim_vortex_joint_p1damping,true},
    {"sim.vortex_joint_p1frictioncoeff",sim_vortex_joint_p1frictioncoeff,true},
    {"sim.vortex_joint_p1frictionmaxforce",sim_vortex_joint_p1frictionmaxforce,true},
    {"sim.vortex_joint_p1frictionloss",sim_vortex_joint_p1frictionloss,true},
    {"sim.vortex_joint_p2loss",sim_vortex_joint_p2loss,true},
    {"sim.vortex_joint_p2stiffness",sim_vortex_joint_p2stiffness,true},
    {"sim.vortex_joint_p2damping",sim_vortex_joint_p2damping,true},
    {"sim.vortex_joint_p2frictioncoeff",sim_vortex_joint_p2frictioncoeff,true},
    {"sim.vortex_joint_p2frictionmaxforce",sim_vortex_joint_p2frictionmaxforce,true},
    {"sim.vortex_joint_p2frictionloss",sim_vortex_joint_p2frictionloss,true},
    {"sim.vortex_joint_a0loss",sim_vortex_joint_a0loss,true},
    {"sim.vortex_joint_a0stiffness",sim_vortex_joint_a0stiffness,true},
    {"sim.vortex_joint_a0damping",sim_vortex_joint_a0damping,true},
    {"sim.vortex_joint_a0frictioncoeff",sim_vortex_joint_a0frictioncoeff,true},
    {"sim.vortex_joint_a0frictionmaxforce",sim_vortex_joint_a0frictionmaxforce,true},
    {"sim.vortex_joint_a0frictionloss",sim_vortex_joint_a0frictionloss,true},
    {"sim.vortex_joint_a1loss",sim_vortex_joint_a1loss,true},
    {"sim.vortex_joint_a1stiffness",sim_vortex_joint_a1stiffness,true},
    {"sim.vortex_joint_a1damping",sim_vortex_joint_a1damping,true},
    {"sim.vortex_joint_a1frictioncoeff",sim_vortex_joint_a1frictioncoeff,true},
    {"sim.vortex_joint_a1frictionmaxforce",sim_vortex_joint_a1frictionmaxforce,true},
    {"sim.vortex_joint_a1frictionloss",sim_vortex_joint_a1frictionloss,true},
    {"sim.vortex_joint_a2loss",sim_vortex_joint_a2loss,true},
    {"sim.vortex_joint_a2stiffness",sim_vortex_joint_a2stiffness,true},
    {"sim.vortex_joint_a2damping",sim_vortex_joint_a2damping,true},
    {"sim.vortex_joint_a2frictioncoeff",sim_vortex_joint_a2frictioncoeff,true},
    {"sim.vortex_joint_a2frictionmaxforce",sim_vortex_joint_a2frictionmaxforce,true},
    {"sim.vortex_joint_a2frictionloss",sim_vortex_joint_a2frictionloss,true},
    {"sim.vortex_joint_dependencyfactor",sim_vortex_joint_dependencyfactor,true},
    {"sim.vortex_joint_dependencyoffset",sim_vortex_joint_dependencyoffset,true},
    {"sim.vortex_joint_bitcoded",sim_vortex_joint_bitcoded,true},
    {"sim.vortex_joint_relaxationenabledbc",sim_vortex_joint_relaxationenabledbc,true},
    {"sim.vortex_joint_frictionenabledbc",sim_vortex_joint_frictionenabledbc,true},
    {"sim.vortex_joint_frictionproportionalbc",sim_vortex_joint_frictionproportionalbc,true},
    {"sim.vortex_joint_objectid",sim_vortex_joint_objectid,true},
    {"sim.vortex_joint_dependentobjectid",sim_vortex_joint_dependentobjectid,true},
    {"sim.vortex_joint_motorfrictionenabled",sim_vortex_joint_motorfrictionenabled,true},
    {"sim.vortex_joint_proportionalmotorfriction",sim_vortex_joint_proportionalmotorfriction,true},
    {"sim.vortex_body_primlinearaxisfriction",sim_vortex_body_primlinearaxisfriction,true},
    {"sim.vortex_body_seclinearaxisfriction",sim_vortex_body_seclinearaxisfriction,true},
    {"sim.vortex_body_primangularaxisfriction",sim_vortex_body_primangularaxisfriction,true},
    {"sim.vortex_body_secangularaxisfriction",sim_vortex_body_secangularaxisfriction,true},
    {"sim.vortex_body_normalangularaxisfriction",sim_vortex_body_normalangularaxisfriction,true},
    {"sim.vortex_body_primlinearaxisstaticfrictionscale",sim_vortex_body_primlinearaxisstaticfrictionscale,true},
    {"sim.vortex_body_seclinearaxisstaticfrictionscale",sim_vortex_body_seclinearaxisstaticfrictionscale,true},
    {"sim.vortex_body_primangularaxisstaticfrictionscale",sim_vortex_body_primangularaxisstaticfrictionscale,true},
    {"sim.vortex_body_secangularaxisstaticfrictionscale",sim_vortex_body_secangularaxisstaticfrictionscale,true},
    {"sim.vortex_body_normalangularaxisstaticfrictionscale",sim_vortex_body_normalangularaxisstaticfrictionscale,true},
    {"sim.vortex_body_compliance",sim_vortex_body_compliance,true},
    {"sim.vortex_body_damping",sim_vortex_body_damping,true},
    {"sim.vortex_body_restitution",sim_vortex_body_restitution,true},
    {"sim.vortex_body_restitutionthreshold",sim_vortex_body_restitutionthreshold,true},
    {"sim.vortex_body_adhesiveforce",sim_vortex_body_adhesiveforce,true},
    {"sim.vortex_body_linearvelocitydamping",sim_vortex_body_linearvelocitydamping,true},
    {"sim.vortex_body_angularvelocitydamping",sim_vortex_body_angularvelocitydamping,true},
    {"sim.vortex_body_primlinearaxisslide",sim_vortex_body_primlinearaxisslide,true},
    {"sim.vortex_body_seclinearaxisslide",sim_vortex_body_seclinearaxisslide,true},
    {"sim.vortex_body_primangularaxisslide",sim_vortex_body_primangularaxisslide,true},
    {"sim.vortex_body_secangularaxisslide",sim_vortex_body_secangularaxisslide,true},
    {"sim.vortex_body_normalangularaxisslide",sim_vortex_body_normalangularaxisslide,true},
    {"sim.vortex_body_primlinearaxisslip",sim_vortex_body_primlinearaxisslip,true},
    {"sim.vortex_body_seclinearaxisslip",sim_vortex_body_seclinearaxisslip,true},
    {"sim.vortex_body_primangularaxisslip",sim_vortex_body_primangularaxisslip,true},
    {"sim.vortex_body_secangularaxisslip",sim_vortex_body_secangularaxisslip,true},
    {"sim.vortex_body_normalangularaxisslip",sim_vortex_body_normalangularaxisslip,true},
    {"sim.vortex_body_autosleeplinearspeedthreshold",sim_vortex_body_autosleeplinearspeedthreshold,true},
    {"sim.vortex_body_autosleeplinearaccelthreshold",sim_vortex_body_autosleeplinearaccelthreshold,true},
    {"sim.vortex_body_autosleepangularspeedthreshold",sim_vortex_body_autosleepangularspeedthreshold,true},
    {"sim.vortex_body_autosleepangularaccelthreshold",sim_vortex_body_autosleepangularaccelthreshold,true},
    {"sim.vortex_body_skinthickness",sim_vortex_body_skinthickness,true},
    {"sim.vortex_body_autoangulardampingtensionratio",sim_vortex_body_autoangulardampingtensionratio,true},
    {"sim.vortex_body_primaxisvectorx",sim_vortex_body_primaxisvectorx,true},
    {"sim.vortex_body_primaxisvectory",sim_vortex_body_primaxisvectory,true},
    {"sim.vortex_body_primaxisvectorz",sim_vortex_body_primaxisvectorz,true},
    {"sim.vortex_body_primlinearaxisfrictionmodel",sim_vortex_body_primlinearaxisfrictionmodel,true},
    {"sim.vortex_body_seclinearaxisfrictionmodel",sim_vortex_body_seclinearaxisfrictionmodel,true},
    {"sim.vortex_body_primangulararaxisfrictionmodel",sim_vortex_body_primangulararaxisfrictionmodel,true},
    {"sim.vortex_body_secmangulararaxisfrictionmodel",sim_vortex_body_secmangulararaxisfrictionmodel,true},
    {"sim.vortex_body_normalmangulararaxisfrictionmodel",sim_vortex_body_normalmangulararaxisfrictionmodel,true},
    {"sim.vortex_body_bitcoded",sim_vortex_body_bitcoded,true},
    {"sim.vortex_body_autosleepsteplivethreshold",sim_vortex_body_autosleepsteplivethreshold,true},
    {"sim.vortex_body_materialuniqueid",sim_vortex_body_materialuniqueid,true},
    {"sim.vortex_body_pureshapesasconvex",sim_vortex_body_pureshapesasconvex,true},
    {"sim.vortex_body_convexshapesasrandom",sim_vortex_body_convexshapesasrandom,true},
    {"sim.vortex_body_randomshapesasterrain",sim_vortex_body_randomshapesasterrain,true},
    {"sim.vortex_body_fastmoving",sim_vortex_body_fastmoving,true},
    {"sim.vortex_body_autoslip",sim_vortex_body_autoslip,true},
    {"sim.vortex_body_seclinaxissameasprimlinaxis",sim_vortex_body_seclinaxissameasprimlinaxis,true},
    {"sim.vortex_body_secangaxissameasprimangaxis",sim_vortex_body_secangaxissameasprimangaxis,true},
    {"sim.vortex_body_normangaxissameasprimangaxis",sim_vortex_body_normangaxissameasprimangaxis,true},
    {"sim.vortex_body_autoangulardamping",sim_vortex_body_autoangulardamping,true},
    // Newton engine params
    {"sim.newton_global_stepsize",sim_newton_global_stepsize,true},
    {"sim.newton_global_contactmergetolerance",sim_newton_global_contactmergetolerance,true},
    {"sim.newton_global_constraintsolvingiterations",sim_newton_global_constraintsolvingiterations,true},
    {"sim.newton_global_bitcoded",sim_newton_global_bitcoded,true},
    {"sim.newton_global_multithreading",sim_newton_global_multithreading,true},
    {"sim.newton_global_exactsolver",sim_newton_global_exactsolver,true},
    {"sim.newton_global_highjointaccuracy",sim_newton_global_highjointaccuracy,true},
    {"sim.newton_joint_dependencyfactor",sim_newton_joint_dependencyfactor,true},
    {"sim.newton_joint_dependencyoffset",sim_newton_joint_dependencyoffset,true},
    {"sim.newton_joint_objectid",sim_newton_joint_objectid,true},
    {"sim.newton_joint_dependentobjectid",sim_newton_joint_dependentobjectid,true},
    {"sim.newton_body_staticfriction",sim_newton_body_staticfriction,true},
    {"sim.newton_body_kineticfriction",sim_newton_body_kineticfriction,true},
    {"sim.newton_body_restitution",sim_newton_body_restitution,true},
    {"sim.newton_body_lineardrag",sim_newton_body_lineardrag,true},
    {"sim.newton_body_angulardrag",sim_newton_body_angulardrag,true},
    {"sim.newton_body_bitcoded",sim_newton_body_bitcoded,true},
    {"sim.newton_body_fastmoving",sim_newton_body_fastmoving,true},
    // Vortex friction models
    {"sim.vortex_bodyfrictionmodel_box",sim_vortex_bodyfrictionmodel_box,true},
    {"sim.vortex_bodyfrictionmodel_scaledbox",sim_vortex_bodyfrictionmodel_scaledbox,true},
    {"sim.vortex_bodyfrictionmodel_proplow",sim_vortex_bodyfrictionmodel_proplow,true},
    {"sim.vortex_bodyfrictionmodel_prophigh",sim_vortex_bodyfrictionmodel_prophigh,true},
    {"sim.vortex_bodyfrictionmodel_scaledboxfast",sim_vortex_bodyfrictionmodel_scaledboxfast,true},
    {"sim.vortex_bodyfrictionmodel_neutral",sim_vortex_bodyfrictionmodel_neutral,true},
    {"sim.vortex_bodyfrictionmodel_none",sim_vortex_bodyfrictionmodel_none,true},
    // Bullet constraint solver types
    {"sim.bullet_constraintsolvertype_sequentialimpulse",sim_bullet_constraintsolvertype_sequentialimpulse,true},
    {"sim.bullet_constraintsolvertype_nncg",sim_bullet_constraintsolvertype_nncg,true},
    {"sim.bullet_constraintsolvertype_dantzig",sim_bullet_constraintsolvertype_dantzig,true},
    {"sim.bullet_constraintsolvertype_projectedgaussseidel",sim_bullet_constraintsolvertype_projectedgaussseidel,true},
    // Filter component types:
    {"sim.filtercomponent_originalimage",sim_filtercomponent_originalimage_deprecated,true},
    {"sim.filtercomponent_originaldepth",sim_filtercomponent_originaldepth_deprecated,true},
    {"sim.filtercomponent_uniformimage",sim_filtercomponent_uniformimage_deprecated,true},
    {"sim.filtercomponent_tooutput",sim_filtercomponent_tooutput_deprecated,true},
    {"sim.filtercomponent_tobuffer1",sim_filtercomponent_tobuffer1_deprecated,true},
    {"sim.filtercomponent_tobuffer2",sim_filtercomponent_tobuffer2_deprecated,true},
    {"sim.filtercomponent_frombuffer1",sim_filtercomponent_frombuffer1_deprecated,true},
    {"sim.filtercomponent_frombuffer2",sim_filtercomponent_frombuffer2_deprecated,true},
    {"sim.filtercomponent_swapbuffers",sim_filtercomponent_swapbuffers_deprecated,true},
    {"sim.filtercomponent_addbuffer1",sim_filtercomponent_addbuffer1_deprecated,true},
    {"sim.filtercomponent_subtractbuffer1",sim_filtercomponent_subtractbuffer1_deprecated,true},
    {"sim.filtercomponent_multiplywithbuffer1",sim_filtercomponent_multiplywithbuffer1_deprecated,true},
    {"sim.filtercomponent_horizontalflip",sim_filtercomponent_horizontalflip_deprecated,true},
    {"sim.filtercomponent_verticalflip",sim_filtercomponent_verticalflip_deprecated,true},
    {"sim.filtercomponent_rotate",sim_filtercomponent_rotate_deprecated,true},
    {"sim.filtercomponent_shift",sim_filtercomponent_shift_deprecated,true},
    {"sim.filtercomponent_resize",sim_filtercomponent_resize_deprecated,true},
    {"sim.filtercomponent_3x3filter",sim_filtercomponent_3x3filter_deprecated,true},
    {"sim.filtercomponent_5x5filter",sim_filtercomponent_5x5filter_deprecated,true},
    {"sim.filtercomponent_sharpen",sim_filtercomponent_sharpen_deprecated,true},
    {"sim.filtercomponent_edge",sim_filtercomponent_edge_deprecated,true},
    {"sim.filtercomponent_rectangularcut",sim_filtercomponent_rectangularcut_deprecated,true},
    {"sim.filtercomponent_circularcut",sim_filtercomponent_circularcut_deprecated,true},
    {"sim.filtercomponent_normalize",sim_filtercomponent_normalize_deprecated,true},
    {"sim.filtercomponent_intensityscale",sim_filtercomponent_intensityscale_deprecated,true},
    {"sim.filtercomponent_keeporremovecolors",sim_filtercomponent_keeporremovecolors_deprecated,true},
    {"sim.filtercomponent_scaleandoffsetcolors",sim_filtercomponent_scaleandoffsetcolors_deprecated,true},
    {"sim.filtercomponent_binary",sim_filtercomponent_binary_deprecated,true},
    {"sim.filtercomponent_swapwithbuffer1",sim_filtercomponent_swapwithbuffer1_deprecated,true},
    {"sim.filtercomponent_addtobuffer1",sim_filtercomponent_addtobuffer1_deprecated,true},
    {"sim.filtercomponent_subtractfrombuffer1",sim_filtercomponent_subtractfrombuffer1_deprecated,true},
    {"sim.filtercomponent_correlationwithbuffer1",sim_filtercomponent_correlationwithbuffer1_deprecated,true},
    {"sim.filtercomponent_colorsegmentation",sim_filtercomponent_colorsegmentation_deprecated,true},
    {"sim.filtercomponent_blobextraction",sim_filtercomponent_blobextraction_deprecated,true},
    {"sim.filtercomponent_imagetocoord",sim_filtercomponent_imagetocoord_deprecated,true},
    {"sim.filtercomponent_pixelchange",sim_filtercomponent_pixelchange_deprecated,true},
    {"sim.filtercomponent_velodyne",sim_filtercomponent_velodyne_deprecated,true},
    {"sim.filtercomponent_todepthoutput",sim_filtercomponent_todepthoutput_deprecated,true},
    {"sim.filtercomponent_customized",sim_filtercomponent_customized_deprecated,true},
    // buffer types:
    {"sim.buffer_uint8",sim_buffer_uint8,true},
    {"sim.buffer_int8",sim_buffer_int8,true},
    {"sim.buffer_uint16",sim_buffer_uint16,true},
    {"sim.buffer_int16",sim_buffer_int16,true},
    {"sim.buffer_uint32",sim_buffer_uint32,true},
    {"sim.buffer_int32",sim_buffer_int32,true},
    {"sim.buffer_float",sim_buffer_float,true},
    {"sim.buffer_double",sim_buffer_double,true},
    {"sim.buffer_uint8rgb",sim_buffer_uint8rgb,true},
    {"sim.buffer_uint8bgr",sim_buffer_uint8bgr,true},
    {"sim.buffer_uint8rgba",sim_buffer_uint8rgba,true},
    {"sim.buffer_uint8argb",sim_buffer_uint8argb,true},
    {"sim.buffer_base64",sim_buffer_base64,true},
    {"sim.buffer_split",sim_buffer_split,true},
    {"sim.buffer_clamp",sim_buffer_clamp,true},
    // Image combination:
    {"sim.imgcomb_vertical",sim_imgcomb_vertical,true},
    {"sim.imgcomb_horizontal",sim_imgcomb_horizontal,true},
    // predefined material types:
    {"sim.dynmat_default",sim_dynmat_default,true},
    {"sim.dynmat_highfriction",sim_dynmat_highfriction,true},
    {"sim.dynmat_lowfriction",sim_dynmat_lowfriction,true},
    {"sim.dynmat_nofriction",sim_dynmat_nofriction,true},
    {"sim.dynmat_reststackgrasp",sim_dynmat_reststackgrasp,true},
    {"sim.dynmat_foot",sim_dynmat_foot,true},
    {"sim.dynmat_wheel",sim_dynmat_wheel,true},
    {"sim.dynmat_gripper",sim_dynmat_gripper,true},
    {"sim.dynmat_floor",sim_dynmat_floor,true},
    {"sim.rml_phase_sync_if_possible",simrml_phase_sync_if_possible,true},
    {"sim.rml_only_time_sync",simrml_only_time_sync,true},
    {"sim.rml_only_phase_sync",simrml_only_phase_sync,true},
    {"sim.rml_no_sync",simrml_no_sync,true},
    {"sim.rml_keep_target_vel",simrml_keep_target_vel,true},
    {"sim.rml_recompute_trajectory",simrml_recompute_trajectory,true},
    {"sim.rml_disable_extremum_motion_states_calc",simrml_disable_extremum_motion_states_calc,true},
    {"sim.rml_keep_current_vel_if_fallback_strategy",simrml_keep_current_vel_if_fallback_strategy,true},


    // deprecated!
    {"sim.jointmode_ik",sim_jointmode_ik_deprecated,false},
    {"sim.jointmode_ikdependent",sim_jointmode_reserved_previously_ikdependent,false},
    {"sim.appobj_ik_type",sim_appobj_ik_type,false},
    // IK calculation methods:
    {"sim.ik_pseudo_inverse_method",sim_ik_pseudo_inverse_method,false},
    {"sim.ik_damped_least_squares_method",sim_ik_damped_least_squares_method,false},
    {"sim.ik_jacobian_transpose_method",sim_ik_jacobian_transpose_method,false},
    {"sim.ik_undamped_pseudo_inverse_method",sim_ik_undamped_pseudo_inverse_method,false},
    // IK constraints:
    {"sim.ik_x_constraint",sim_ik_x_constraint,false},
    {"sim.ik_y_constraint",sim_ik_y_constraint,false},
    {"sim.ik_z_constraint",sim_ik_z_constraint,false},
    {"sim.ik_alpha_beta_constraint",sim_ik_alpha_beta_constraint,false},
    {"sim.ik_gamma_constraint",sim_ik_gamma_constraint,false},
    // IK calculation results:
    {"sim.ikresult_not_performed",sim_ikresult_not_performed,false},
    {"sim.ikresult_success",sim_ikresult_success,false},
    {"sim.ikresult_fail",sim_ikresult_fail,false},

    {"sim.dummy_linktype_gcs_loop_closure",sim_dummy_linktype_gcs_loop_closure,false},
    {"sim.dummy_linktype_gcs_tip",sim_dummy_linktype_gcs_tip,false},
    {"sim.dummy_linktype_gcs_target",sim_dummy_linktype_gcs_target,false},
    {"sim.dummy_linktype_ik_tip_target",sim_dummy_linktype_ik_tip_target,false},
    {"sim.dummy_linktype_dynamics_force_constraint",sim_dummy_linktype_dynamics_force_constraint,false},
    {"sim.jointfloatparam_step_size",sim_jointfloatparam_step_size,false},
    {"sim.jointfloatparam_ik_weight",sim_jointfloatparam_ik_weight,false},
    {"sim.boolparam_ik_handling_enabled",sim_boolparam_ik_handling_enabled,false},
    {"sim.boolparam_gcs_handling_enabled",sim_boolparam_gcs_handling_enabled,false},
    {"sim.appobj_2delement_type",sim_appobj_ui_type,false},  // for backward compatibility
    {"sim.appobj_ui_type",sim_appobj_ui_type,false},
    {"sim.appobj_pathplanning_type",sim_appobj_pathplanning_type,false},
    {"sim.scripttype_threaded",sim_scripttype_threaded_old,false},
    {"sim.navigation_camerafly",sim_navigation_camerafly_old,false},

    {"",-1}
};


const SLuaVariables simLuaVariablesOldApi[]=
{ // Following for backward compatibility (see newer equivalent commands ('simLuaVariables'))
    {"sim.mainscriptcall_initialization",sim_syscb_init,false},
    {"sim.mainscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim.mainscriptcall_regular",sim_syscb_actuation,false},
    {"sim.syscb_regular",sim_syscb_actuation,false},
    {"sim.childscriptcall_initialization",sim_syscb_init,false},
    {"sim.childscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim.childscriptcall_actuation",sim_syscb_actuation,false},
    {"sim.childscriptcall_sensing",sim_syscb_sensing,false},
    {"sim.addonscriptcall_initialization",sim_syscb_init,false},
    {"sim.addonscriptcall_suspend",sim_syscb_aos_suspend,false},
    {"sim.addonscriptcall_restarting",sim_syscb_aos_resume,false},
    {"sim.addonscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim.customizationscriptcall_initialization",sim_syscb_init,false},
    {"sim.customizationscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim.customizationscriptcall_nonsimulation",sim_syscb_nonsimulation,false},
    {"sim.customizationscriptcall_lastbeforesimulation",sim_syscb_beforesimulation,false},
    {"sim.customizationscriptcall_firstaftersimulation",sim_syscb_aftersimulation,false},
    {"sim.customizationscriptcall_simulationactuation",sim_syscb_actuation,false},
    {"sim.customizationscriptcall_simulationsensing",sim_syscb_sensing,false},
    {"sim.customizationscriptcall_simulationpause",sim_syscb_suspended,false},
    {"sim.customizationscriptcall_simulationpausefirst",sim_syscb_suspend,false},
    {"sim.customizationscriptcall_simulationpauselast",sim_syscb_resume,false},
    {"sim.customizationscriptcall_lastbeforeinstanceswitch",sim_syscb_beforeinstanceswitch,false},
    {"sim.customizationscriptcall_firstafterinstanceswitch",sim_syscb_afterinstanceswitch,false},
    {"sim.customizationscriptcall_beforecopy",sim_syscb_beforecopy,false},
    {"sim.customizationscriptcall_aftercopy",sim_syscb_aftercopy,false},
    {"sim.customizationscriptcall_br",sim_syscb_br,false},
    {"sim.boolparam_show_w_emitters",sim_boolparam_show_w_emitters,false},
    {"sim.boolparam_show_w_receivers",sim_boolparam_show_w_receivers,false},

    {"sim_object_shape_type",sim_object_shape_type,false},
    {"sim_object_joint_type",sim_object_joint_type,false},
    {"sim_object_graph_type",sim_object_graph_type,false},
    {"sim_object_camera_type",sim_object_camera_type,false},
    {"sim_object_dummy_type",sim_object_dummy_type,false},
    {"sim_object_proximitysensor_type",sim_object_proximitysensor_type,false},
    {"sim_object_path_type",sim_object_path_type,false},
    {"sim_object_renderingsensor_type",sim_object_visionsensor_type,false},
    {"sim_object_visionsensor_type",sim_object_visionsensor_type,false},
    {"sim_object_mill_type",sim_object_mill_type,false},
    {"sim_object_forcesensor_type",sim_object_forcesensor_type,false},
    {"sim_object_light_type",sim_object_light_type,false},
    {"sim_object_mirror_type",sim_object_mirror_type,false},
    {"sim_object_octree_type",sim_object_octree_type,false},
    {"sim_object_pointcloud_type",sim_object_pointcloud_type,false},
    {"sim_light_omnidirectional_subtype",sim_light_omnidirectional_subtype,false},
    {"sim_light_spot_subtype",sim_light_spot_subtype,false},
    {"sim_light_directional_subtype",sim_light_directional_subtype,false},
    {"sim_joint_revolute_subtype",sim_joint_revolute_subtype,false},
    {"sim_joint_prismatic_subtype",sim_joint_prismatic_subtype,false},
    {"sim_joint_spherical_subtype",sim_joint_spherical_subtype,false},
    {"sim_shape_simpleshape_subtype",sim_shape_simpleshape_subtype,false},
    {"sim_shape_multishape_subtype",sim_shape_multishape_subtype,false},
    {"sim_proximitysensor_pyramid_subtype",sim_proximitysensor_pyramid_subtype,false},
    {"sim_proximitysensor_cylinder_subtype",sim_proximitysensor_cylinder_subtype,false},
    {"sim_proximitysensor_disc_subtype",sim_proximitysensor_disc_subtype,false},
    {"sim_proximitysensor_cone_subtype",sim_proximitysensor_cone_subtype,false},
    {"sim_proximitysensor_ray_subtype",sim_proximitysensor_ray_subtype,false},
    {"sim_mill_pyramid_subtype",sim_mill_pyramid_subtype,false},
    {"sim_mill_cylinder_subtype",sim_mill_cylinder_subtype,false},
    {"sim_mill_disc_subtype",sim_mill_disc_subtype,false},
    {"sim_mill_cone_subtype",sim_mill_cone_subtype,false},
    {"sim_object_no_subtype",sim_object_no_subtype,false},
    {"sim_appobj_object_type",sim_appobj_object_type,false},
    {"sim_appobj_collision_type",sim_appobj_collision_type,false},
    {"sim_appobj_distance_type",sim_appobj_distance_type,false},
    {"sim_appobj_simulation_type",sim_appobj_simulation_type,false},
    {"sim_appobj_ik_type",sim_appobj_ik_type,false},
    {"sim_appobj_collection_type",sim_appobj_collection_type,false},
    {"sim_appobj_2delement_type",sim_appobj_ui_type,false},  // for backward compatibility
    {"sim_appobj_ui_type",sim_appobj_ui_type,false},
    {"sim_appobj_script_type",sim_appobj_script_type,false},
    {"sim_appobj_pathplanning_type",sim_appobj_pathplanning_type,false},
    {"sim_appobj_texture_type",sim_appobj_texture_type,false},
    {"sim_ik_pseudo_inverse_method",sim_ik_pseudo_inverse_method,false},
    {"sim_ik_damped_least_squares_method",sim_ik_damped_least_squares_method,false},
    {"sim_ik_jacobian_transpose_method",sim_ik_jacobian_transpose_method,false},
    {"sim_ik_x_constraint",sim_ik_x_constraint,false},
    {"sim_ik_y_constraint",sim_ik_y_constraint,false},
    {"sim_ik_z_constraint",sim_ik_z_constraint,false},
    {"sim_ik_alpha_beta_constraint",sim_ik_alpha_beta_constraint,false},
    {"sim_ik_gamma_constraint",sim_ik_gamma_constraint,false},
    {"sim_ikresult_not_performed",sim_ikresult_not_performed,false},
    {"sim_ikresult_success",sim_ikresult_success,false},
    {"sim_ikresult_fail",sim_ikresult_fail,false},
    {"sim_message_ui_button_state_change",sim_message_ui_button_state_change,false},
    {"sim_message_model_loaded",sim_message_model_loaded,false},
    {"sim_message_scene_loaded",sim_message_scene_loaded,false},
    {"sim_message_object_selection_changed",sim_message_object_selection_changed,false},
    {"sim_message_keypress",sim_message_keypress,false},
    {"sim_message_bannerclicked",sim_message_bannerclicked,false},
    {"sim_message_prox_sensor_select_down",sim_message_prox_sensor_select_down,false},
    {"sim_message_prox_sensor_select_up",sim_message_prox_sensor_select_up,false},
    {"sim_message_pick_select_down",sim_message_pick_select_down,false},
    {"sim_buttonproperty_button",sim_buttonproperty_button,false},
    {"sim_buttonproperty_label",sim_buttonproperty_label,false},
    {"sim_buttonproperty_editbox",sim_buttonproperty_editbox,false},
    {"sim_buttonproperty_slider",sim_buttonproperty_slider,false},
    {"sim_buttonproperty_staydown",sim_buttonproperty_staydown,false},
    {"sim_buttonproperty_enabled",sim_buttonproperty_enabled,false},
    {"sim_buttonproperty_borderless",sim_buttonproperty_borderless,false},
    {"sim_buttonproperty_horizontallycentered",sim_buttonproperty_horizontallycentered,false},
    {"sim_buttonproperty_ignoremouse",sim_buttonproperty_ignoremouse,false},
    {"sim_buttonproperty_isdown",sim_buttonproperty_isdown,false},
    {"sim_buttonproperty_transparent",sim_buttonproperty_transparent,false},
    {"sim_buttonproperty_nobackgroundcolor",sim_buttonproperty_nobackgroundcolor,false},
    {"sim_buttonproperty_rollupaction",sim_buttonproperty_rollupaction,false},
    {"sim_buttonproperty_closeaction",sim_buttonproperty_closeaction,false},
    {"sim_buttonproperty_verticallycentered",sim_buttonproperty_verticallycentered,false},
    {"sim_buttonproperty_downupevent",sim_buttonproperty_downupevent,false},
    {"sim_objectproperty_collapsed",sim_objectproperty_collapsed,false},
    {"sim_objectproperty_selectable",sim_objectproperty_selectable,false},
    {"sim_objectproperty_selectmodelbaseinstead",sim_objectproperty_selectmodelbaseinstead,false},
    {"sim_objectproperty_dontshowasinsidemodel",sim_objectproperty_dontshowasinsidemodel,false},
    {"sim_objectproperty_canupdatedna",sim_objectproperty_canupdatedna,false},
    {"sim_objectproperty_selectinvisible",sim_objectproperty_selectinvisible,false},
    {"sim_objectproperty_depthinvisible",sim_objectproperty_depthinvisible,false},
    {"sim_objectproperty_cannotdelete",sim_objectproperty_cannotdelete,false},
    {"sim_simulation_stopped",sim_simulation_stopped,false},
    {"sim_simulation_paused",sim_simulation_paused,false},
    {"sim_simulation_advancing",sim_simulation_advancing,false},
    {"sim_simulation_advancing_firstafterstop",sim_simulation_advancing_firstafterstop,false},
    {"sim_simulation_advancing_running",sim_simulation_advancing_running,false},
    {"sim_simulation_advancing_lastbeforepause",sim_simulation_advancing_lastbeforepause,false},
    {"sim_simulation_advancing_firstafterpause",sim_simulation_advancing_firstafterpause,false},
    {"sim_simulation_advancing_abouttostop",sim_simulation_advancing_abouttostop,false},
    {"sim_simulation_advancing_lastbeforestop",sim_simulation_advancing_lastbeforestop,false},
    {"sim_texturemap_plane",sim_texturemap_plane,false},
    {"sim_texturemap_cylinder",sim_texturemap_cylinder,false},
    {"sim_texturemap_sphere",sim_texturemap_sphere,false},
    {"sim_texturemap_cube",sim_texturemap_cube,false},
    {"sim_scripttype_mainscript",sim_scripttype_mainscript,false},
    {"sim_scripttype_childscript",sim_scripttype_childscript,false},
    {"sim_scripttype_addonscript",sim_scripttype_addonscript,false},
    {"sim_scripttype_addonfunction",sim_scripttype_addonfunction,false},
    {"sim_scripttype_customizationscript",sim_scripttype_customizationscript,false},
    {"sim_scripttype_threaded",sim_scripttype_threaded_old,false},
    {"sim_mainscriptcall_initialization",sim_syscb_init,false},
    {"sim_mainscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim_mainscriptcall_regular",sim_syscb_actuation,false},
    {"sim_childscriptcall_initialization",sim_syscb_init,false},
    {"sim_childscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim_childscriptcall_actuation",sim_syscb_actuation,false},
    {"sim_childscriptcall_sensing",sim_syscb_sensing,false},
    {"sim_childscriptcall_threaded",-1,false},
    {"sim_customizationscriptcall_initialization",sim_syscb_init,false},
    {"sim_customizationscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim_customizationscriptcall_nonsimulation",sim_syscb_nonsimulation,false},
    {"sim_customizationscriptcall_lastbeforesimulation",sim_syscb_beforesimulation,false},
    {"sim_customizationscriptcall_firstaftersimulation",sim_syscb_aftersimulation,false},
    {"sim_customizationscriptcall_simulationactuation",sim_syscb_actuation,false},
    {"sim_customizationscriptcall_simulationsensing",sim_syscb_sensing,false},
    {"sim_customizationscriptcall_simulationpause",sim_syscb_suspended,false},
    {"sim_customizationscriptcall_simulationpausefirst",sim_syscb_suspend,false},
    {"sim_customizationscriptcall_simulationpauselast",sim_syscb_resume,false},
    {"sim_customizationscriptcall_lastbeforeinstanceswitch",sim_syscb_beforeinstanceswitch,false},
    {"sim_customizationscriptcall_firstafterinstanceswitch",sim_syscb_afterinstanceswitch,false},
    {"sim_addonscriptcall_initialization",sim_syscb_init,false},
    {"sim_addonscriptcall_suspend",sim_syscb_aos_suspend,false},
    {"sim_addonscriptcall_restarting",sim_syscb_aos_resume,false},
    {"sim_addonscriptcall_cleanup",sim_syscb_cleanup,false},
    {"sim_customizationscriptattribute_activeduringsimulation",sim_customizationscriptattribute_activeduringsimulation,false},
    {"sim_scriptattribute_executionorder",sim_scriptattribute_executionorder,false},
    {"sim_scriptattribute_executioncount",sim_scriptattribute_executioncount,false},
    {"sim_childscriptattribute_automaticcascadingcalls",sim_childscriptattribute_automaticcascadingcalls,false},
    {"sim_childscriptattribute_enabled",sim_childscriptattribute_enabled,false},
    {"sim_scriptattribute_enabled",sim_scriptattribute_enabled,false},
    {"sim_customizationscriptattribute_cleanupbeforesave",sim_customizationscriptattribute_cleanupbeforesave,false},
    {"sim_scriptexecorder_first",sim_scriptexecorder_first,false},
    {"sim_scriptexecorder_normal",sim_scriptexecorder_normal,false},
    {"sim_scriptexecorder_last",sim_scriptexecorder_last,false},
    {"sim_scriptthreadresume_allnotyetresumed",sim_scriptthreadresume_allnotyetresumed,false},
    {"sim_scriptthreadresume_default",sim_scriptthreadresume_default,false},
    {"sim_scriptthreadresume_actuation_first",sim_scriptthreadresume_actuation_first,false},
    {"sim_scriptthreadresume_actuation_last",sim_scriptthreadresume_actuation_last,false},
    {"sim_scriptthreadresume_sensing_first",sim_scriptthreadresume_sensing_first,false},
    {"sim_scriptthreadresume_sensing_last",sim_scriptthreadresume_sensing_last,false},
    {"sim_scriptthreadresume_custom",sim_scriptthreadresume_custom,false},
    {"sim_callbackid_rossubscriber",sim_callbackid_rossubscriber,false},
    {"sim_callbackid_dynstep",sim_callbackid_dynstep,false},
    {"sim_callbackid_userdefined",sim_callbackid_userdefined,false},
    {"sim_script_no_error",sim_script_no_error,false},
    {"sim_script_main_script_nonexistent",sim_script_main_script_nonexistent,false},
    {"sim_script_main_not_called",sim_script_main_script_not_called,false},
    {"sim_script_reentrance_error",sim_script_reentrance_error,false},
    {"sim_script_lua_error",sim_script_lua_error,false},
    {"sim_script_call_error",sim_script_call_error,false},
    {"sim_api_error_report",sim_api_error_report,false},
    {"sim_api_error_output",sim_api_error_output,false},
    {"sim_api_warning_output",sim_api_warning_output,false},
    {"sim_handle_all",sim_handle_all,false},
    {"sim_handle_all_except_explicit",sim_handle_all_except_explicit,false},
    {"sim_handle_self",sim_handle_self,false},
    {"sim_handle_main_script",sim_handle_main_script,false},
    {"sim_handle_tree",sim_handle_tree,false},
    {"sim_handle_chain",sim_handle_chain,false},
    {"sim_handle_single",sim_handle_single,false},
    {"sim_handle_default",sim_handle_default,false},
    {"sim_handle_all_except_self",sim_handle_all_except_self,false},
    {"sim_handle_parent",sim_handle_parent,false},
    {"sim_handle_scene",sim_handle_scene,false},
    {"sim_handle_app",sim_handle_app,false},
    {"sim_handleflag_assembly",sim_handleflag_assembly,false},
    {"sim_handleflag_camera",sim_handleflag_camera,false},
    {"sim_handleflag_togglevisibility",sim_handleflag_togglevisibility,false},
    {"sim_handleflag_extended",sim_handleflag_extended,false},
    {"sim_handleflag_greyscale",sim_handleflag_greyscale,false},
    {"sim_handleflag_codedstring",sim_handleflag_codedstring,false},
    {"sim_handleflag_model",sim_handleflag_model,false},
    {"sim_handleflag_rawvalue",sim_handleflag_rawvalue,false},
    {"sim_objectspecialproperty_collidable",sim_objectspecialproperty_collidable,false},
    {"sim_objectspecialproperty_measurable",sim_objectspecialproperty_measurable,false},
    {"sim_objectspecialproperty_detectable_ultrasonic",sim_objectspecialproperty_detectable_ultrasonic,false},
    {"sim_objectspecialproperty_detectable_infrared",sim_objectspecialproperty_detectable_infrared,false},
    {"sim_objectspecialproperty_detectable_laser",sim_objectspecialproperty_detectable_laser,false},
    {"sim_objectspecialproperty_detectable_inductive",sim_objectspecialproperty_detectable_inductive,false},
    {"sim_objectspecialproperty_detectable_capacitive",sim_objectspecialproperty_detectable_capacitive,false},
    {"sim_objectspecialproperty_renderable",sim_objectspecialproperty_renderable,false},
    {"sim_objectspecialproperty_detectable_all",sim_objectspecialproperty_detectable_all,false},
    {"sim_objectspecialproperty_pathplanning_ignored",sim_objectspecialproperty_pathplanning_ignored,false},
    {"sim_modelproperty_not_collidable",sim_modelproperty_not_collidable,false},
    {"sim_modelproperty_not_measurable",sim_modelproperty_not_measurable,false},
    {"sim_modelproperty_not_renderable",sim_modelproperty_not_renderable,false},
    {"sim_modelproperty_not_detectable",sim_modelproperty_not_detectable,false},
    {"sim_modelproperty_not_dynamic",sim_modelproperty_not_dynamic,false},
    {"sim_modelproperty_not_respondable",sim_modelproperty_not_respondable,false},
    {"sim_modelproperty_not_reset",sim_modelproperty_not_reset,false},
    {"sim_modelproperty_not_visible",sim_modelproperty_not_visible,false},
    {"sim_modelproperty_scripts_inactive",sim_modelproperty_scripts_inactive,false},
    {"sim_modelproperty_not_showasinsidemodel",sim_modelproperty_not_showasinsidemodel,false},
    {"sim_modelproperty_not_model",sim_modelproperty_not_model,false},
    {"sim_dlgstyle_message",sim_dlgstyle_message,false},
    {"sim_dlgstyle_input",sim_dlgstyle_input,false},
    {"sim_dlgstyle_ok",sim_dlgstyle_ok,false},
    {"sim_dlgstyle_ok_cancel",sim_dlgstyle_ok_cancel,false},
    {"sim_dlgstyle_yes_no",sim_dlgstyle_yes_no,false},
    {"sim_dlgstyle_dont_center",sim_dlgstyle_dont_center,false},
    {"sim_dlgret_still_open",sim_dlgret_still_open,false},
    {"sim_dlgret_ok",sim_dlgret_ok,false},
    {"sim_dlgret_cancel",sim_dlgret_cancel,false},
    {"sim_dlgret_yes",sim_dlgret_yes,false},
    {"sim_dlgret_no",sim_dlgret_no,false},
    {"sim_pathproperty_show_line",sim_pathproperty_show_line,false},
    {"sim_pathproperty_show_orientation",sim_pathproperty_show_orientation,false},
    {"sim_pathproperty_closed_path",sim_pathproperty_closed_path,false},
    {"sim_pathproperty_automatic_orientation",sim_pathproperty_automatic_orientation,false},
    {"sim_pathproperty_flat_path",sim_pathproperty_flat_path,false},
    {"sim_pathproperty_show_position",sim_pathproperty_show_position,false},
    {"sim_pathproperty_keep_x_up",sim_pathproperty_keep_x_up,false},
    {"sim_distcalcmethod_dl",sim_distcalcmethod_dl,false},
    {"sim_distcalcmethod_dac",sim_distcalcmethod_dac,false},
    {"sim_distcalcmethod_max_dl_dac",sim_distcalcmethod_max_dl_dac,false},
    {"sim_distcalcmethod_dl_and_dac",sim_distcalcmethod_dl_and_dac,false},
    {"sim_distcalcmethod_sqrt_dl2_and_dac2",sim_distcalcmethod_sqrt_dl2_and_dac2,false},
    {"sim_distcalcmethod_dl_if_nonzero",sim_distcalcmethod_dl_if_nonzero,false},
    {"sim_distcalcmethod_dac_if_nonzero",sim_distcalcmethod_dac_if_nonzero,false},
    {"sim_boolparam_hierarchy_visible",sim_boolparam_hierarchy_visible,false},
    {"sim_boolparam_console_visible",sim_boolparam_console_visible,false},
    {"sim_boolparam_collision_handling_enabled",sim_boolparam_collision_handling_enabled,false},
    {"sim_boolparam_distance_handling_enabled",sim_boolparam_distance_handling_enabled,false},
    {"sim_boolparam_ik_handling_enabled",sim_boolparam_ik_handling_enabled,false},
    {"sim_boolparam_gcs_handling_enabled",sim_boolparam_gcs_handling_enabled,false},
    {"sim_boolparam_dynamics_handling_enabled",sim_boolparam_dynamics_handling_enabled,false},
    {"sim_boolparam_proximity_sensor_handling_enabled",sim_boolparam_proximity_sensor_handling_enabled,false},
    {"sim_boolparam_vision_sensor_handling_enabled",sim_boolparam_vision_sensor_handling_enabled,false},
    {"sim_boolparam_rendering_sensor_handling_enabled",sim_boolparam_vision_sensor_handling_enabled,false},
    {"sim_boolparam_mill_handling_enabled",sim_boolparam_mill_handling_enabled,false},
    {"sim_boolparam_browser_visible",sim_boolparam_browser_visible,false},
    {"sim_boolparam_scene_and_model_load_messages",sim_boolparam_scene_and_model_load_messages,false},
    {"sim_boolparam_shape_textures_are_visible",sim_boolparam_shape_textures_are_visible,false},
    {"sim_boolparam_display_enabled",sim_boolparam_display_enabled,false},
    {"sim_boolparam_infotext_visible",sim_boolparam_infotext_visible,false},
    {"sim_boolparam_statustext_open",sim_boolparam_statustext_open,false},
    {"sim_boolparam_fog_enabled",sim_boolparam_fog_enabled,false},
    {"sim_boolparam_rml2_available",sim_boolparam_rml2_available,false},
    {"sim_boolparam_rml4_available",sim_boolparam_rml4_available,false},
    {"sim_boolparam_mirrors_enabled",sim_boolparam_mirrors_enabled,false},
    {"sim_boolparam_aux_clip_planes_enabled",sim_boolparam_aux_clip_planes_enabled,false},
    {"sim_boolparam_full_model_copy_from_api",sim_boolparam_reserved3,false},
    {"sim_boolparam_realtime_simulation",sim_boolparam_realtime_simulation,false},
    {"sim_boolparam_use_glfinish_cmd",sim_boolparam_use_glfinish_cmd,false},
    {"sim_boolparam_force_show_wireless_emission",sim_boolparam_force_show_wireless_emission,false},
    {"sim_boolparam_force_show_wireless_reception",sim_boolparam_force_show_wireless_reception,false},
    {"sim_boolparam_video_recording_triggered",sim_boolparam_video_recording_triggered,false},
    {"sim_boolparam_fullscreen",sim_boolparam_fullscreen,false},
    {"sim_boolparam_headless",sim_boolparam_headless,false},
    {"sim_boolparam_hierarchy_toolbarbutton_enabled",sim_boolparam_hierarchy_toolbarbutton_enabled,false},
    {"sim_boolparam_browser_toolbarbutton_enabled",sim_boolparam_browser_toolbarbutton_enabled,false},
    {"sim_boolparam_objectshift_toolbarbutton_enabled",sim_boolparam_objectshift_toolbarbutton_enabled,false},
    {"sim_boolparam_objectrotate_toolbarbutton_enabled",sim_boolparam_objectrotate_toolbarbutton_enabled,false},
    {"sim_boolparam_force_calcstruct_all_visible",sim_boolparam_force_calcstruct_all_visible,false},
    {"sim_boolparam_force_calcstruct_all",sim_boolparam_force_calcstruct_all,false},
    {"sim_boolparam_exit_request",sim_boolparam_exit_request,false},
    {"sim_boolparam_play_toolbarbutton_enabled",sim_boolparam_play_toolbarbutton_enabled,false},
    {"sim_boolparam_pause_toolbarbutton_enabled",sim_boolparam_pause_toolbarbutton_enabled,false},
    {"sim_boolparam_stop_toolbarbutton_enabled",sim_boolparam_stop_toolbarbutton_enabled,false},
    {"sim_boolparam_waiting_for_trigger",sim_boolparam_waiting_for_trigger,false},
    {"sim_boolparam_objproperties_toolbarbutton_enabled",sim_boolparam_objproperties_toolbarbutton_enabled,false},
    {"sim_boolparam_calcmodules_toolbarbutton_enabled",sim_boolparam_calcmodules_toolbarbutton_enabled,false},
    {"sim_boolparam_rosinterface_donotrunmainscript",sim_boolparam_rosinterface_donotrunmainscript,false},
    {"sim_intparam_error_report_mode",sim_intparam_error_report_mode,false},
    {"sim_intparam_program_version",sim_intparam_program_version,false},
    {"sim_intparam_compilation_version",sim_intparam_compilation_version,false},
    {"sim_intparam_current_page",sim_intparam_current_page,false},
    {"sim_intparam_flymode_camera_handle",sim_intparam_flymode_camera_handle,false},
    {"sim_intparam_dynamic_step_divider",sim_intparam_dynamic_step_divider,false},
    {"sim_intparam_dynamic_engine",sim_intparam_dynamic_engine,false},
    {"sim_intparam_server_port_start",sim_intparam_server_port_start,false},
    {"sim_intparam_server_port_range",sim_intparam_server_port_range,false},
    {"sim_intparam_server_port_next",sim_intparam_server_port_next,false},
    {"sim_intparam_visible_layers",sim_intparam_visible_layers,false},
    {"sim_intparam_infotext_style",sim_intparam_infotext_style,false},
    {"sim_intparam_settings",sim_intparam_settings,false},
    {"sim_intparam_qt_version",sim_intparam_qt_version,false},
    {"sim_intparam_platform",sim_intparam_platform,false},
    {"sim_intparam_scene_unique_id",sim_intparam_scene_unique_id,false},
    {"sim_intparam_edit_mode_type",sim_intparam_edit_mode_type,false},
    {"sim_intparam_work_thread_count",sim_intparam_work_thread_count,false}, // deprecated
    {"sim_intparam_mouse_x",sim_intparam_mouse_x,false},
    {"sim_intparam_mouse_y",sim_intparam_mouse_y,false},
    {"sim_intparam_core_count",sim_intparam_core_count,false},
    {"sim_intparam_work_thread_calc_time_ms",sim_intparam_work_thread_calc_time_ms,false}, // deprecated
    {"sim_intparam_idle_fps",sim_intparam_idle_fps,false},
    {"sim_intparam_prox_sensor_select_down",sim_intparam_prox_sensor_select_down,false},
    {"sim_intparam_prox_sensor_select_up",sim_intparam_prox_sensor_select_up,false},
    {"sim_intparam_stop_request_counter",sim_intparam_stop_request_counter,false},
    {"sim_intparam_program_revision",sim_intparam_program_revision,false},
    {"sim_intparam_mouse_buttons",sim_intparam_mouse_buttons,false},
    {"sim_intparam_dynamic_warning_disabled_mask",sim_intparam_dynamic_warning_disabled_mask,false},
    {"sim_intparam_simulation_warning_disabled_mask",sim_intparam_simulation_warning_disabled_mask,false},
    {"sim_intparam_scene_index",sim_intparam_scene_index,false},
    {"sim_intparam_motionplanning_seed",sim_intparam_motionplanning_seed,false},
    {"sim_intparam_speedmodifier",sim_intparam_speedmodifier,false},
    {"sim_intparam_dynamic_iteration_count",sim_intparam_dynamic_iteration_count,false},
    {"sim_floatparam_rand",sim_floatparam_rand,false},
    {"sim_floatparam_simulation_time_step",sim_floatparam_simulation_time_step,false},
    {"sim_floatparam_stereo_distance",sim_floatparam_stereo_distance,false},
    {"sim_floatparam_dynamic_step_size",sim_floatparam_dynamic_step_size,false},
    {"sim_floatparam_mouse_wheel_zoom_factor",sim_floatparam_mouse_wheel_zoom_factor,false},
    {"sim_arrayparam_gravity",sim_arrayparam_gravity,false},
    {"sim_arrayparam_fog",sim_arrayparam_fog,false},
    {"sim_arrayparam_fog_color",sim_arrayparam_fog_color,false},
    {"sim_arrayparam_background_color1",sim_arrayparam_background_color1,false},
    {"sim_arrayparam_background_color2",sim_arrayparam_background_color2,false},
    {"sim_arrayparam_ambient_light",sim_arrayparam_ambient_light,false},
    {"sim_arrayparam_random_euler",sim_arrayparam_random_euler,false},
    {"sim_stringparam_application_path",sim_stringparam_application_path,false},
    {"sim_stringparam_video_filename",sim_stringparam_video_filename,false},
    {"sim_stringparam_app_arg1",sim_stringparam_app_arg1,false},
    {"sim_stringparam_app_arg2",sim_stringparam_app_arg2,false},
    {"sim_stringparam_app_arg3",sim_stringparam_app_arg3,false},
    {"sim_stringparam_app_arg4",sim_stringparam_app_arg4,false},
    {"sim_stringparam_app_arg5",sim_stringparam_app_arg5,false},
    {"sim_stringparam_app_arg6",sim_stringparam_app_arg6,false},
    {"sim_stringparam_app_arg7",sim_stringparam_app_arg7,false},
    {"sim_stringparam_app_arg8",sim_stringparam_app_arg8,false},
    {"sim_stringparam_app_arg9",sim_stringparam_app_arg9,false},
    {"sim_stringparam_scene_path_and_name",sim_stringparam_scene_path_and_name,false},
    {"sim_stringparam_remoteapi_temp_file_dir",sim_stringparam_remoteapi_temp_file_dir,false},
    {"sim_stringparam_scene_path",sim_stringparam_scene_path,false},
    {"sim_stringparam_scene_name",sim_stringparam_scene_name,false},
    {"sim_displayattribute_renderpass",sim_displayattribute_renderpass,false},
    {"sim_displayattribute_depthpass",sim_displayattribute_depthpass,false},
    {"sim_displayattribute_pickpass",sim_displayattribute_pickpass,false},
    {"sim_displayattribute_selected",sim_displayattribute_selected,false},
    {"sim_displayattribute_mainselection",sim_displayattribute_mainselection,false},
    {"sim_displayattribute_forcewireframe",sim_displayattribute_forcewireframe,false},
    {"sim_displayattribute_forbidwireframe",sim_displayattribute_forbidwireframe,false},
    {"sim_displayattribute_forbidedges",sim_displayattribute_forbidedges,false},
    {"sim_displayattribute_originalcolors",sim_displayattribute_originalcolors,false},
    {"sim_displayattribute_ignorelayer",sim_displayattribute_ignorelayer,false},
    {"sim_displayattribute_forvisionsensor",sim_displayattribute_forvisionsensor,false},
    {"sim_displayattribute_colorcodedpickpass",sim_displayattribute_colorcodedpickpass,false},
    {"sim_displayattribute_colorcoded",sim_displayattribute_colorcoded,false},
    {"sim_displayattribute_trianglewireframe",sim_displayattribute_trianglewireframe,false},
    {"sim_displayattribute_thickEdges",sim_displayattribute_thickEdges,false},
    {"sim_displayattribute_dynamiccontentonly",sim_displayattribute_dynamiccontentonly,false},
    {"sim_displayattribute_mirror",sim_displayattribute_mirror,false},
    {"sim_displayattribute_useauxcomponent",sim_displayattribute_useauxcomponent,false},
    {"sim_displayattribute_ignorerenderableflag",sim_displayattribute_ignorerenderableflag,false},
    {"sim_displayattribute_noopenglcallbacks",sim_displayattribute_noopenglcallbacks,false},
    {"sim_displayattribute_noghosts",sim_displayattribute_noghosts,false},
    {"sim_displayattribute_nopointclouds",sim_displayattribute_nopointclouds,false},
    {"sim_displayattribute_nodrawingobjects",sim_displayattribute_nodrawingobjects,false},
    {"sim_displayattribute_noparticles",sim_displayattribute_noparticles,false},
    {"sim_displayattribute_colorcodedtriangles",sim_displayattribute_colorcodedtriangles,false},
    {"sim_navigation_passive",sim_navigation_passive,false},
    {"sim_navigation_camerashift",sim_navigation_camerashift,false},
    {"sim_navigation_camerarotate",sim_navigation_camerarotate,false},
    {"sim_navigation_camerazoom",sim_navigation_camerazoom,false},
    {"sim_navigation_cameratilt",sim_navigation_cameratilt,false},
    {"sim_navigation_cameraangle",sim_navigation_cameraangle,false},
    {"sim_navigation_objectshift",sim_navigation_objectshift,false},
    {"sim_navigation_objectrotate",sim_navigation_objectrotate,false},
    {"sim_navigation_createpathpoint",sim_navigation_createpathpoint,false},
    {"sim_navigation_clickselection",sim_navigation_clickselection,false},
    {"sim_navigation_ctrlselection",sim_navigation_ctrlselection,false},
    {"sim_navigation_shiftselection",sim_navigation_shiftselection,false},
    {"sim_navigation_camerazoomwheel",sim_navigation_camerazoomwheel,false},
    {"sim_navigation_camerarotaterightbutton",sim_navigation_camerarotaterightbutton,false},
    {"sim_navigation_camerarotatemiddlebutton",sim_navigation_camerarotatemiddlebutton,false},
    {"sim_drawing_points",sim_drawing_points,false},
    {"sim_drawing_lines",sim_drawing_lines,false},
    {"sim_drawing_triangles",sim_drawing_triangles,false},
    {"sim_drawing_trianglepoints",sim_drawing_trianglepoints,false},
    {"sim_drawing_quadpoints",sim_drawing_quadpoints,false},
    {"sim_drawing_discpoints",sim_drawing_discpoints,false},
    {"sim_drawing_cubepoints",sim_drawing_cubepoints,false},
    {"sim_drawing_spherepoints",sim_drawing_spherepoints,false},
    {"sim_drawing_itemcolors",sim_drawing_itemcolors,false},
    {"sim_drawing_vertexcolors",sim_drawing_vertexcolors,false},
    {"sim_drawing_itemsizes",sim_drawing_itemsizes,false},
    {"sim_drawing_backfaceculling",sim_drawing_backfaceculling,false},
    {"sim_drawing_wireframe",sim_drawing_wireframe,false},
    {"sim_drawing_painttag",sim_drawing_painttag,false},
    {"sim_drawing_followparentvisibility",sim_drawing_followparentvisibility,false},
    {"sim_drawing_cyclic",sim_drawing_cyclic,false},
    {"sim_drawing_50percenttransparency",sim_drawing_50percenttransparency,false},
    {"sim_drawing_25percenttransparency",sim_drawing_25percenttransparency,false},
    {"sim_drawing_12percenttransparency",sim_drawing_12percenttransparency,false},
    {"sim_drawing_emissioncolor",sim_drawing_emissioncolor,false},
    {"sim_drawing_facingcamera",sim_drawing_facingcamera,false},
    {"sim_drawing_overlay",sim_drawing_overlay,false},
    {"sim_drawing_itemtransparency",sim_drawing_itemtransparency,false},
    {"sim_drawing_persistent",sim_drawing_persistent,false},
    {"sim_drawing_auxchannelcolor1",sim_drawing_auxchannelcolor1,false},
    {"sim_drawing_auxchannelcolor2",sim_drawing_auxchannelcolor2,false},
    {"sim_banner_left",sim_banner_left,false},
    {"sim_banner_right",sim_banner_right,false},
    {"sim_banner_nobackground",sim_banner_nobackground,false},
    {"sim_banner_overlay",sim_banner_overlay,false},
    {"sim_banner_followparentvisibility",sim_banner_followparentvisibility,false},
    {"sim_banner_clickselectsparent",sim_banner_clickselectsparent,false},
    {"sim_banner_clicktriggersevent",sim_banner_clicktriggersevent,false},
    {"sim_banner_facingcamera",sim_banner_facingcamera,false},
    {"sim_banner_fullyfacingcamera",sim_banner_fullyfacingcamera,false},
    {"sim_banner_backfaceculling",sim_banner_backfaceculling,false},
    {"sim_banner_keepsamesize",sim_banner_keepsamesize,false},
    {"sim_banner_bitmapfont",sim_banner_bitmapfont,false},
    {"sim_particle_points1",sim_particle_points1,false},
    {"sim_particle_points2",sim_particle_points2,false},
    {"sim_particle_points4",sim_particle_points4,false},
    {"sim_particle_roughspheres",sim_particle_roughspheres,false},
    {"sim_particle_spheres",sim_particle_spheres,false},
    {"sim_particle_respondable1to4",sim_particle_respondable1to4,false},
    {"sim_particle_respondable5to8",sim_particle_respondable5to8,false},
    {"sim_particle_particlerespondable",sim_particle_particlerespondable,false},
    {"sim_particle_ignoresgravity",sim_particle_ignoresgravity,false},
    {"sim_particle_invisible",sim_particle_invisible,false},
    {"sim_particle_painttag",sim_particle_painttag,false},
    {"sim_particle_itemsizes",sim_particle_itemsizes,false},
    {"sim_particle_itemdensities",sim_particle_itemdensities,false},
    {"sim_particle_itemcolors",sim_particle_itemcolors,false},
    {"sim_particle_cyclic",sim_particle_cyclic,false},
    {"sim_particle_emissioncolor",sim_particle_emissioncolor,false},
    {"sim_particle_water",sim_particle_water,false},
    {"sim_jointmode_passive",sim_jointmode_passive,false},
    {"sim_jointmode_ik",sim_jointmode_ik_deprecated,false},
    {"sim_jointmode_ikdependent",sim_jointmode_reserved_previously_ikdependent,false},
    {"sim_jointmode_dependent",sim_jointmode_dependent,false},
    {"sim_jointmode_force",sim_jointmode_force,false},
    {"sim_filedlg_type_load",sim_filedlg_type_load,false},
    {"sim_filedlg_type_save",sim_filedlg_type_save,false},
    {"sim_filedlg_type_load_multiple",sim_filedlg_type_load_multiple,false},
    {"sim_filedlg_type_folder",sim_filedlg_type_folder,false},
    {"sim_msgbox_type_info",sim_msgbox_type_info,false},
    {"sim_msgbox_type_question",sim_msgbox_type_question,false},
    {"sim_msgbox_type_warning",sim_msgbox_type_warning,false},
    {"sim_msgbox_type_critical",sim_msgbox_type_critical,false},
    {"sim_msgbox_buttons_ok",sim_msgbox_buttons_ok,false},
    {"sim_msgbox_buttons_yesno",sim_msgbox_buttons_yesno,false},
    {"sim_msgbox_buttons_yesnocancel",sim_msgbox_buttons_yesnocancel,false},
    {"sim_msgbox_buttons_okcancel",sim_msgbox_buttons_okcancel,false},
    {"sim_msgbox_return_cancel",sim_msgbox_return_cancel,false},
    {"sim_msgbox_return_no",sim_msgbox_return_no,false},
    {"sim_msgbox_return_yes",sim_msgbox_return_yes,false},
    {"sim_msgbox_return_ok",sim_msgbox_return_ok,false},
    {"sim_msgbox_return_error",sim_msgbox_return_error,false},
    {"sim_physics_bullet",sim_physics_bullet,false},
    {"sim_physics_ode",sim_physics_ode,false},
    {"sim_physics_vortex",sim_physics_vortex,false},
    {"sim_physics_newton",sim_physics_newton,false},
    {"sim_pure_primitive_none",sim_pure_primitive_none,false},
    {"sim_pure_primitive_plane",sim_pure_primitive_plane,false},
    {"sim_pure_primitive_disc",sim_pure_primitive_disc,false},
    {"sim_pure_primitive_cuboid",sim_pure_primitive_cuboid,false},
    {"sim_pure_primitive_spheroid",sim_pure_primitive_spheroid,false},
    {"sim_pure_primitive_cylinder",sim_pure_primitive_cylinder,false},
    {"sim_pure_primitive_cone",sim_pure_primitive_cone,false},
    {"sim_pure_primitive_heightfield",sim_pure_primitive_heightfield,false},
    {"sim_dummy_linktype_dynamics_loop_closure",sim_dummy_linktype_dynamics_loop_closure,false},
    {"sim_dummy_linktype_dynamics_force_constraint",sim_dummy_linktype_dynamics_force_constraint,false},
    {"sim_dummy_linktype_gcs_loop_closure",sim_dummy_linktype_gcs_loop_closure,false},
    {"sim_dummy_linktype_gcs_tip",sim_dummy_linktype_gcs_tip,false},
    {"sim_dummy_linktype_gcs_target",sim_dummy_linktype_gcs_target,false},
    {"sim_dummy_linktype_ik_tip_target",sim_dummy_linktype_ik_tip_target,false},
    {"sim_colorcomponent_ambient",sim_colorcomponent_ambient,false},
    {"sim_colorcomponent_ambient_diffuse",sim_colorcomponent_ambient_diffuse,false},
    {"sim_colorcomponent_diffuse",sim_colorcomponent_diffuse,false},
    {"sim_colorcomponent_specular",sim_colorcomponent_specular,false},
    {"sim_colorcomponent_emission",sim_colorcomponent_emission,false},
    {"sim_colorcomponent_transparency",sim_colorcomponent_transparency,false},
    {"sim_colorcomponent_auxiliary",sim_colorcomponent_auxiliary,false},
    {"sim_volume_ray",sim_volume_ray,false},
    {"sim_volume_randomizedray",sim_volume_randomizedray,false},
    {"sim_volume_pyramid",sim_volume_pyramid,false},
    {"sim_volume_cylinder",sim_volume_cylinder,false},
    {"sim_volume_disc",sim_volume_disc,false},
    {"sim_volume_cone",sim_volume_cone,false},
    {"sim_objintparam_visibility_layer",sim_objintparam_visibility_layer,false},
    {"sim_objfloatparam_abs_x_velocity",sim_objfloatparam_abs_x_velocity,false},
    {"sim_objfloatparam_abs_y_velocity",sim_objfloatparam_abs_y_velocity,false},
    {"sim_objfloatparam_abs_z_velocity",sim_objfloatparam_abs_z_velocity,false},
    {"sim_objfloatparam_abs_rot_velocity",sim_objfloatparam_abs_rot_velocity,false},
    {"sim_objfloatparam_objbbox_min_x",sim_objfloatparam_objbbox_min_x,false},
    {"sim_objfloatparam_objbbox_min_y",sim_objfloatparam_objbbox_min_y,false},
    {"sim_objfloatparam_objbbox_min_z",sim_objfloatparam_objbbox_min_z,false},
    {"sim_objfloatparam_objbbox_max_x",sim_objfloatparam_objbbox_max_x,false},
    {"sim_objfloatparam_objbbox_max_y",sim_objfloatparam_objbbox_max_y,false},
    {"sim_objfloatparam_objbbox_max_z",sim_objfloatparam_objbbox_max_z,false},
    {"sim_objfloatparam_modelbbox_min_x",sim_objfloatparam_modelbbox_min_x,false},
    {"sim_objfloatparam_modelbbox_min_y",sim_objfloatparam_modelbbox_min_y,false},
    {"sim_objfloatparam_modelbbox_min_z",sim_objfloatparam_modelbbox_min_z,false},
    {"sim_objfloatparam_modelbbox_max_x",sim_objfloatparam_modelbbox_max_x,false},
    {"sim_objfloatparam_modelbbox_max_y",sim_objfloatparam_modelbbox_max_y,false},
    {"sim_objfloatparam_modelbbox_max_z",sim_objfloatparam_modelbbox_max_z,false},
    {"sim_objintparam_collection_self_collision_indicator",sim_objintparam_collection_self_collision_indicator,false},
    {"sim_objfloatparam_transparency_offset",sim_objfloatparam_transparency_offset,false},
    {"sim_objintparam_child_role",sim_objintparam_child_role,false},
    {"sim_objintparam_parent_role",sim_objintparam_parent_role,false},
    {"sim_objintparam_manipulation_permissions",sim_objintparam_manipulation_permissions,false},
    {"sim_objintparam_illumination_handle",sim_objintparam_illumination_handle,false},
    {"sim_objstringparam_dna",sim_objstringparam_dna,false},
    {"sim_visionfloatparam_near_clipping",sim_visionfloatparam_near_clipping,false},
    {"sim_visionfloatparam_far_clipping",sim_visionfloatparam_far_clipping,false},
    {"sim_visionintparam_resolution_x",sim_visionintparam_resolution_x,false},
    {"sim_visionintparam_resolution_y",sim_visionintparam_resolution_y,false},
    {"sim_visionfloatparam_perspective_angle",sim_visionfloatparam_perspective_angle,false},
    {"sim_visionfloatparam_ortho_size",sim_visionfloatparam_ortho_size,false},
    {"sim_visionintparam_disabled_light_components",sim_visionintparam_disabled_light_components,false},
    {"sim_visionintparam_rendering_attributes",sim_visionintparam_rendering_attributes,false},
    {"sim_visionintparam_entity_to_render",sim_visionintparam_entity_to_render,false},
    {"sim_visionintparam_windowed_size_x",sim_visionintparam_windowed_size_x,false},
    {"sim_visionintparam_windowed_size_y",sim_visionintparam_windowed_size_y,false},
    {"sim_visionintparam_windowed_pos_x",sim_visionintparam_windowed_pos_x,false},
    {"sim_visionintparam_windowed_pos_y",sim_visionintparam_windowed_pos_y,false},
    {"sim_visionintparam_pov_focal_blur",sim_visionintparam_pov_focal_blur,false},
    {"sim_visionfloatparam_pov_blur_distance",sim_visionfloatparam_pov_blur_distance,false},
    {"sim_visionfloatparam_pov_aperture",sim_visionfloatparam_pov_aperture,false},
    {"sim_visionintparam_pov_blur_sampled",sim_visionintparam_pov_blur_sampled,false},
    {"sim_visionintparam_render_mode",sim_visionintparam_render_mode,false},
    {"sim_jointintparam_motor_enabled",sim_jointintparam_motor_enabled,false},
    {"sim_jointintparam_ctrl_enabled",sim_jointintparam_ctrl_enabled,false},
    {"sim_jointfloatparam_pid_p",sim_jointfloatparam_pid_p,false},
    {"sim_jointfloatparam_pid_i",sim_jointfloatparam_pid_i,false},
    {"sim_jointfloatparam_pid_d",sim_jointfloatparam_pid_d,false},
    {"sim_jointfloatparam_intrinsic_x",sim_jointfloatparam_intrinsic_x,false},
    {"sim_jointfloatparam_intrinsic_y",sim_jointfloatparam_intrinsic_y,false},
    {"sim_jointfloatparam_intrinsic_z",sim_jointfloatparam_intrinsic_z,false},
    {"sim_jointfloatparam_intrinsic_qx",sim_jointfloatparam_intrinsic_qx,false},
    {"sim_jointfloatparam_intrinsic_qy",sim_jointfloatparam_intrinsic_qy,false},
    {"sim_jointfloatparam_intrinsic_qz",sim_jointfloatparam_intrinsic_qz,false},
    {"sim_jointfloatparam_intrinsic_qw",sim_jointfloatparam_intrinsic_qw,false},
    {"sim_jointfloatparam_velocity",sim_jointfloatparam_velocity,false},
    {"sim_jointfloatparam_spherical_qx",sim_jointfloatparam_spherical_qx,false},
    {"sim_jointfloatparam_spherical_qy",sim_jointfloatparam_spherical_qy,false},
    {"sim_jointfloatparam_spherical_qz",sim_jointfloatparam_spherical_qz,false},
    {"sim_jointfloatparam_spherical_qw",sim_jointfloatparam_spherical_qw,false},
    {"sim_jointfloatparam_upper_limit",sim_jointfloatparam_upper_limit,false},
    {"sim_jointfloatparam_kc_k",sim_jointfloatparam_kc_k,false},
    {"sim_jointfloatparam_kc_c",sim_jointfloatparam_kc_c,false},
    {"sim_jointfloatparam_ik_weight",sim_jointfloatparam_ik_weight,false},
    {"sim_jointfloatparam_error_x",sim_jointfloatparam_error_x,false},
    {"sim_jointfloatparam_error_y",sim_jointfloatparam_error_y,false},
    {"sim_jointfloatparam_error_z",sim_jointfloatparam_error_z,false},
    {"sim_jointfloatparam_error_a",sim_jointfloatparam_error_a,false},
    {"sim_jointfloatparam_error_b",sim_jointfloatparam_error_b,false},
    {"sim_jointfloatparam_error_g",sim_jointfloatparam_error_g,false},
    {"sim_jointfloatparam_error_pos",sim_jointfloatparam_error_pos,false},
    {"sim_jointfloatparam_error_angle",sim_jointfloatparam_error_angle,false},
    {"sim_jointintparam_velocity_lock",sim_jointintparam_velocity_lock,false},
    {"sim_jointintparam_vortex_dep_handle",sim_jointintparam_vortex_dep_handle,false},
    {"sim_jointfloatparam_vortex_dep_multiplication",sim_jointfloatparam_vortex_dep_multiplication,false},
    {"sim_jointfloatparam_vortex_dep_offset",sim_jointfloatparam_vortex_dep_offset,false},
    {"sim_shapefloatparam_init_velocity_x",sim_shapefloatparam_init_velocity_x,false},
    {"sim_shapefloatparam_init_velocity_y",sim_shapefloatparam_init_velocity_y,false},
    {"sim_shapefloatparam_init_velocity_z",sim_shapefloatparam_init_velocity_z,false},
    {"sim_shapeintparam_static",sim_shapeintparam_static,false},
    {"sim_shapeintparam_respondable",sim_shapeintparam_respondable,false},
    {"sim_shapefloatparam_mass",sim_shapefloatparam_mass,false},
    {"sim_shapefloatparam_texture_x",sim_shapefloatparam_texture_x,false},
    {"sim_shapefloatparam_texture_y",sim_shapefloatparam_texture_y,false},
    {"sim_shapefloatparam_texture_z",sim_shapefloatparam_texture_z,false},
    {"sim_shapefloatparam_texture_a",sim_shapefloatparam_texture_a,false},
    {"sim_shapefloatparam_texture_b",sim_shapefloatparam_texture_b,false},
    {"sim_shapefloatparam_texture_g",sim_shapefloatparam_texture_g,false},
    {"sim_shapefloatparam_texture_scaling_x",sim_shapefloatparam_texture_scaling_x,false},
    {"sim_shapefloatparam_texture_scaling_y",sim_shapefloatparam_texture_scaling_y,false},
    {"sim_shapeintparam_culling",sim_shapeintparam_culling,false},
    {"sim_shapeintparam_wireframe",sim_shapeintparam_wireframe,false},
    {"sim_shapeintparam_compound",sim_shapeintparam_compound,false},
    {"sim_shapeintparam_convex",sim_shapeintparam_convex,false},
    {"sim_shapeintparam_convex_check",sim_shapeintparam_convex_check,false},
    {"sim_shapeintparam_respondable_mask",sim_shapeintparam_respondable_mask,false},
    {"sim_shapefloatparam_init_velocity_a",sim_shapefloatparam_init_ang_velocity_x,false},
    {"sim_shapefloatparam_init_velocity_b",sim_shapefloatparam_init_ang_velocity_y,false},
    {"sim_shapefloatparam_init_velocity_g",sim_shapefloatparam_init_ang_velocity_z,false},
    {"sim_shapestringparam_color_name",sim_shapestringparam_color_name,false},
    {"sim_shapeintparam_edge_visibility",sim_shapeintparam_edge_visibility,false},
    {"sim_shapefloatparam_shading_angle",sim_shapefloatparam_shading_angle,false},
    {"sim_shapefloatparam_edge_angle",sim_shapefloatparam_edge_angle,false},
    {"sim_shapeintparam_edge_borders_hidden",sim_shapeintparam_edge_borders_hidden,false},
    {"sim_proxintparam_ray_invisibility",sim_proxintparam_ray_invisibility,false},
    {"sim_proxintparam_volume_type",sim_proxintparam_volume_type,false},
    {"sim_proxintparam_entity_to_detect",sim_proxintparam_entity_to_detect,false},
    {"sim_forcefloatparam_error_x",sim_forcefloatparam_error_x,false},
    {"sim_forcefloatparam_error_y",sim_forcefloatparam_error_y,false},
    {"sim_forcefloatparam_error_z",sim_forcefloatparam_error_z,false},
    {"sim_forcefloatparam_error_a",sim_forcefloatparam_error_a,false},
    {"sim_forcefloatparam_error_b",sim_forcefloatparam_error_b,false},
    {"sim_forcefloatparam_error_g",sim_forcefloatparam_error_g,false},
    {"sim_forcefloatparam_error_pos",sim_forcefloatparam_error_pos,false},
    {"sim_forcefloatparam_error_angle",sim_forcefloatparam_error_angle,false},
    {"sim_lightintparam_pov_casts_shadows",sim_lightintparam_pov_casts_shadows,false},
    {"sim_cameraintparam_disabled_light_components",sim_cameraintparam_disabled_light_components,false},
    {"sim_camerafloatparam_perspective_angle",sim_camerafloatparam_perspective_angle,false},
    {"sim_camerafloatparam_ortho_size",sim_camerafloatparam_ortho_size,false},
    {"sim_cameraintparam_rendering_attributes",sim_cameraintparam_rendering_attributes,false},
    {"sim_cameraintparam_pov_focal_blur",sim_cameraintparam_pov_focal_blur,false},
    {"sim_camerafloatparam_pov_blur_distance",sim_camerafloatparam_pov_blur_distance,false},
    {"sim_camerafloatparam_pov_aperture",sim_camerafloatparam_pov_aperture,false},
    {"sim_cameraintparam_pov_blur_samples",sim_cameraintparam_pov_blur_samples,false},
    {"sim_dummyintparam_link_type",sim_dummyintparam_link_type,false},
    {"sim_dummyintparam_follow_path",sim_dummyintparam_follow_path,false},
    {"sim_dummyfloatparam_follow_path_offset",sim_dummyfloatparam_follow_path_offset,false},
    {"sim_millintparam_volume_type",sim_millintparam_volume_type,false},
    {"sim_mirrorfloatparam_width",sim_mirrorfloatparam_width,false},
    {"sim_mirrorfloatparam_height",sim_mirrorfloatparam_height,false},
    {"sim_mirrorfloatparam_reflectance",sim_mirrorfloatparam_reflectance,false},
    {"sim_mirrorintparam_enable",sim_mirrorintparam_enable,false},
    {"sim_bullet_global_stepsize",sim_bullet_global_stepsize,false},
    {"sim_bullet_global_internalscalingfactor",sim_bullet_global_internalscalingfactor,false},
    {"sim_bullet_global_collisionmarginfactor",sim_bullet_global_collisionmarginfactor,false},
    {"sim_bullet_global_constraintsolvingiterations",sim_bullet_global_constraintsolvingiterations,false},
    {"sim_bullet_global_bitcoded",sim_bullet_global_bitcoded,false},
    {"sim_bullet_global_constraintsolvertype",sim_bullet_global_constraintsolvertype,false},
    {"sim_bullet_global_fullinternalscaling",sim_bullet_global_fullinternalscaling,false},
    {"sim_bullet_joint_stoperp",sim_bullet_joint_stoperp,false},
    {"sim_bullet_joint_stopcfm",sim_bullet_joint_stopcfm,false},
    {"sim_bullet_joint_normalcfm",sim_bullet_joint_normalcfm,false},
    {"sim_bullet_body_restitution",sim_bullet_body_restitution,false},
    {"sim_bullet_body_oldfriction",sim_bullet_body_oldfriction,false},
    {"sim_bullet_body_friction",sim_bullet_body_friction,false},
    {"sim_bullet_body_lineardamping",sim_bullet_body_lineardamping,false},
    {"sim_bullet_body_angulardamping",sim_bullet_body_angulardamping,false},
    {"sim_bullet_body_nondefaultcollisionmargingfactor",sim_bullet_body_nondefaultcollisionmargingfactor,false},
    {"sim_bullet_body_nondefaultcollisionmargingfactorconvex",sim_bullet_body_nondefaultcollisionmargingfactorconvex,false},
    {"sim_bullet_body_bitcoded",sim_bullet_body_bitcoded,false},
    {"sim_bullet_body_sticky",sim_bullet_body_sticky,false},
    {"sim_bullet_body_usenondefaultcollisionmargin",sim_bullet_body_usenondefaultcollisionmargin,false},
    {"sim_bullet_body_usenondefaultcollisionmarginconvex",sim_bullet_body_usenondefaultcollisionmarginconvex,false},
    {"sim_bullet_body_autoshrinkconvex",sim_bullet_body_autoshrinkconvex,false},
    {"sim_ode_global_stepsize",sim_ode_global_stepsize,false},
    {"sim_ode_global_internalscalingfactor",sim_ode_global_internalscalingfactor,false},
    {"sim_ode_global_cfm",sim_ode_global_cfm,false},
    {"sim_ode_global_erp",sim_ode_global_erp,false},
    {"sim_ode_global_constraintsolvingiterations",sim_ode_global_constraintsolvingiterations,false},
    {"sim_ode_global_bitcoded",sim_ode_global_bitcoded,false},
    {"sim_ode_global_randomseed",sim_ode_global_randomseed,false},
    {"sim_ode_global_fullinternalscaling",sim_ode_global_fullinternalscaling,false},
    {"sim_ode_global_quickstep",sim_ode_global_quickstep,false},
    {"sim_ode_joint_stoperp",sim_ode_joint_stoperp,false},
    {"sim_ode_joint_stopcfm",sim_ode_joint_stopcfm,false},
    {"sim_ode_joint_bounce",sim_ode_joint_bounce,false},
    {"sim_ode_joint_fudgefactor",sim_ode_joint_fudgefactor,false},
    {"sim_ode_joint_normalcfm",sim_ode_joint_normalcfm,false},
    {"sim_ode_body_friction",sim_ode_body_friction,false},
    {"sim_ode_body_softerp",sim_ode_body_softerp,false},
    {"sim_ode_body_softcfm",sim_ode_body_softcfm,false},
    {"sim_ode_body_lineardamping",sim_ode_body_lineardamping,false},
    {"sim_ode_body_angulardamping",sim_ode_body_angulardamping,false},
    {"sim_ode_body_maxcontacts",sim_ode_body_maxcontacts,false},
    {"sim_vortex_global_stepsize",sim_vortex_global_stepsize,false},
    {"sim_vortex_global_internalscalingfactor",sim_vortex_global_internalscalingfactor,false},
    {"sim_vortex_global_contacttolerance",sim_vortex_global_contacttolerance,false},
    {"sim_vortex_global_constraintlinearcompliance",sim_vortex_global_constraintlinearcompliance,false},
    {"sim_vortex_global_constraintlineardamping",sim_vortex_global_constraintlineardamping,false},
    {"sim_vortex_global_constraintlinearkineticloss",sim_vortex_global_constraintlinearkineticloss,false},
    {"sim_vortex_global_constraintangularcompliance",sim_vortex_global_constraintangularcompliance,false},
    {"sim_vortex_global_constraintangulardamping",sim_vortex_global_constraintangulardamping,false},
    {"sim_vortex_global_constraintangularkineticloss",sim_vortex_global_constraintangularkineticloss,false},
    {"sim_vortex_global_bitcoded",sim_vortex_global_bitcoded,false},
    {"sim_vortex_global_autosleep",sim_vortex_global_autosleep,false},
    {"sim_vortex_global_multithreading",sim_vortex_global_multithreading,false},
    {"sim_vortex_joint_lowerlimitdamping",sim_vortex_joint_lowerlimitdamping,false},
    {"sim_vortex_joint_upperlimitdamping",sim_vortex_joint_upperlimitdamping,false},
    {"sim_vortex_joint_lowerlimitstiffness",sim_vortex_joint_lowerlimitstiffness,false},
    {"sim_vortex_joint_upperlimitstiffness",sim_vortex_joint_upperlimitstiffness,false},
    {"sim_vortex_joint_lowerlimitrestitution",sim_vortex_joint_lowerlimitrestitution,false},
    {"sim_vortex_joint_upperlimitrestitution",sim_vortex_joint_upperlimitrestitution,false},
    {"sim_vortex_joint_lowerlimitmaxforce",sim_vortex_joint_lowerlimitmaxforce,false},
    {"sim_vortex_joint_upperlimitmaxforce",sim_vortex_joint_upperlimitmaxforce,false},
    {"sim_vortex_joint_motorconstraintfrictioncoeff",sim_vortex_joint_motorconstraintfrictioncoeff,false},
    {"sim_vortex_joint_motorconstraintfrictionmaxforce",sim_vortex_joint_motorconstraintfrictionmaxforce,false},
    {"sim_vortex_joint_motorconstraintfrictionloss",sim_vortex_joint_motorconstraintfrictionloss,false},
    {"sim_vortex_joint_p0loss",sim_vortex_joint_p0loss,false},
    {"sim_vortex_joint_p0stiffness",sim_vortex_joint_p0stiffness,false},
    {"sim_vortex_joint_p0damping",sim_vortex_joint_p0damping,false},
    {"sim_vortex_joint_p0frictioncoeff",sim_vortex_joint_p0frictioncoeff,false},
    {"sim_vortex_joint_p0frictionmaxforce",sim_vortex_joint_p0frictionmaxforce,false},
    {"sim_vortex_joint_p0frictionloss",sim_vortex_joint_p0frictionloss,false},
    {"sim_vortex_joint_p1loss",sim_vortex_joint_p1loss,false},
    {"sim_vortex_joint_p1stiffness",sim_vortex_joint_p1stiffness,false},
    {"sim_vortex_joint_p1damping",sim_vortex_joint_p1damping,false},
    {"sim_vortex_joint_p1frictioncoeff",sim_vortex_joint_p1frictioncoeff,false},
    {"sim_vortex_joint_p1frictionmaxforce",sim_vortex_joint_p1frictionmaxforce,false},
    {"sim_vortex_joint_p1frictionloss",sim_vortex_joint_p1frictionloss,false},
    {"sim_vortex_joint_p2loss",sim_vortex_joint_p2loss,false},
    {"sim_vortex_joint_p2stiffness",sim_vortex_joint_p2stiffness,false},
    {"sim_vortex_joint_p2damping",sim_vortex_joint_p2damping,false},
    {"sim_vortex_joint_p2frictioncoeff",sim_vortex_joint_p2frictioncoeff,false},
    {"sim_vortex_joint_p2frictionmaxforce",sim_vortex_joint_p2frictionmaxforce,false},
    {"sim_vortex_joint_p2frictionloss",sim_vortex_joint_p2frictionloss,false},
    {"sim_vortex_joint_a0loss",sim_vortex_joint_a0loss,false},
    {"sim_vortex_joint_a0stiffness",sim_vortex_joint_a0stiffness,false},
    {"sim_vortex_joint_a0damping",sim_vortex_joint_a0damping,false},
    {"sim_vortex_joint_a0frictioncoeff",sim_vortex_joint_a0frictioncoeff,false},
    {"sim_vortex_joint_a0frictionmaxforce",sim_vortex_joint_a0frictionmaxforce,false},
    {"sim_vortex_joint_a0frictionloss",sim_vortex_joint_a0frictionloss,false},
    {"sim_vortex_joint_a1loss",sim_vortex_joint_a1loss,false},
    {"sim_vortex_joint_a1stiffness",sim_vortex_joint_a1stiffness,false},
    {"sim_vortex_joint_a1damping",sim_vortex_joint_a1damping,false},
    {"sim_vortex_joint_a1frictioncoeff",sim_vortex_joint_a1frictioncoeff,false},
    {"sim_vortex_joint_a1frictionmaxforce",sim_vortex_joint_a1frictionmaxforce,false},
    {"sim_vortex_joint_a1frictionloss",sim_vortex_joint_a1frictionloss,false},
    {"sim_vortex_joint_a2loss",sim_vortex_joint_a2loss,false},
    {"sim_vortex_joint_a2stiffness",sim_vortex_joint_a2stiffness,false},
    {"sim_vortex_joint_a2damping",sim_vortex_joint_a2damping,false},
    {"sim_vortex_joint_a2frictioncoeff",sim_vortex_joint_a2frictioncoeff,false},
    {"sim_vortex_joint_a2frictionmaxforce",sim_vortex_joint_a2frictionmaxforce,false},
    {"sim_vortex_joint_a2frictionloss",sim_vortex_joint_a2frictionloss,false},
    {"sim_vortex_joint_dependencyfactor",sim_vortex_joint_dependencyfactor,false},
    {"sim_vortex_joint_dependencyoffset",sim_vortex_joint_dependencyoffset,false},
    {"sim_vortex_joint_bitcoded",sim_vortex_joint_bitcoded,false},
    {"sim_vortex_joint_relaxationenabledbc",sim_vortex_joint_relaxationenabledbc,false},
    {"sim_vortex_joint_frictionenabledbc",sim_vortex_joint_frictionenabledbc,false},
    {"sim_vortex_joint_frictionproportionalbc",sim_vortex_joint_frictionproportionalbc,false},
    {"sim_vortex_joint_objectid",sim_vortex_joint_objectid,false},
    {"sim_vortex_joint_dependentobjectid",sim_vortex_joint_dependentobjectid,false},
    {"sim_vortex_joint_motorfrictionenabled",sim_vortex_joint_motorfrictionenabled,false},
    {"sim_vortex_joint_proportionalmotorfriction",sim_vortex_joint_proportionalmotorfriction,false},
    {"sim_vortex_body_primlinearaxisfriction",sim_vortex_body_primlinearaxisfriction,false},
    {"sim_vortex_body_seclinearaxisfriction",sim_vortex_body_seclinearaxisfriction,false},
    {"sim_vortex_body_primangularaxisfriction",sim_vortex_body_primangularaxisfriction,false},
    {"sim_vortex_body_secangularaxisfriction",sim_vortex_body_secangularaxisfriction,false},
    {"sim_vortex_body_normalangularaxisfriction",sim_vortex_body_normalangularaxisfriction,false},
    {"sim_vortex_body_primlinearaxisstaticfrictionscale",sim_vortex_body_primlinearaxisstaticfrictionscale,false},
    {"sim_vortex_body_seclinearaxisstaticfrictionscale",sim_vortex_body_seclinearaxisstaticfrictionscale,false},
    {"sim_vortex_body_primangularaxisstaticfrictionscale",sim_vortex_body_primangularaxisstaticfrictionscale,false},
    {"sim_vortex_body_secangularaxisstaticfrictionscale",sim_vortex_body_secangularaxisstaticfrictionscale,false},
    {"sim_vortex_body_normalangularaxisstaticfrictionscale",sim_vortex_body_normalangularaxisstaticfrictionscale,false},
    {"sim_vortex_body_compliance",sim_vortex_body_compliance,false},
    {"sim_vortex_body_damping",sim_vortex_body_damping,false},
    {"sim_vortex_body_restitution",sim_vortex_body_restitution,false},
    {"sim_vortex_body_restitutionthreshold",sim_vortex_body_restitutionthreshold,false},
    {"sim_vortex_body_adhesiveforce",sim_vortex_body_adhesiveforce,false},
    {"sim_vortex_body_linearvelocitydamping",sim_vortex_body_linearvelocitydamping,false},
    {"sim_vortex_body_angularvelocitydamping",sim_vortex_body_angularvelocitydamping,false},
    {"sim_vortex_body_primlinearaxisslide",sim_vortex_body_primlinearaxisslide,false},
    {"sim_vortex_body_seclinearaxisslide",sim_vortex_body_seclinearaxisslide,false},
    {"sim_vortex_body_primangularaxisslide",sim_vortex_body_primangularaxisslide,false},
    {"sim_vortex_body_secangularaxisslide",sim_vortex_body_secangularaxisslide,false},
    {"sim_vortex_body_normalangularaxisslide",sim_vortex_body_normalangularaxisslide,false},
    {"sim_vortex_body_primlinearaxisslip",sim_vortex_body_primlinearaxisslip,false},
    {"sim_vortex_body_seclinearaxisslip",sim_vortex_body_seclinearaxisslip,false},
    {"sim_vortex_body_primangularaxisslip",sim_vortex_body_primangularaxisslip,false},
    {"sim_vortex_body_secangularaxisslip",sim_vortex_body_secangularaxisslip,false},
    {"sim_vortex_body_normalangularaxisslip",sim_vortex_body_normalangularaxisslip,false},
    {"sim_vortex_body_autosleeplinearspeedthreshold",sim_vortex_body_autosleeplinearspeedthreshold,false},
    {"sim_vortex_body_autosleeplinearaccelthreshold",sim_vortex_body_autosleeplinearaccelthreshold,false},
    {"sim_vortex_body_autosleepangularspeedthreshold",sim_vortex_body_autosleepangularspeedthreshold,false},
    {"sim_vortex_body_autosleepangularaccelthreshold",sim_vortex_body_autosleepangularaccelthreshold,false},
    {"sim_vortex_body_skinthickness",sim_vortex_body_skinthickness,false},
    {"sim_vortex_body_autoangulardampingtensionratio",sim_vortex_body_autoangulardampingtensionratio,false},
    {"sim_vortex_body_primaxisvectorx",sim_vortex_body_primaxisvectorx,false},
    {"sim_vortex_body_primaxisvectory",sim_vortex_body_primaxisvectory,false},
    {"sim_vortex_body_primaxisvectorz",sim_vortex_body_primaxisvectorz,false},
    {"sim_vortex_body_primlinearaxisfrictionmodel",sim_vortex_body_primlinearaxisfrictionmodel,false},
    {"sim_vortex_body_seclinearaxisfrictionmodel",sim_vortex_body_seclinearaxisfrictionmodel,false},
    {"sim_vortex_body_primangulararaxisfrictionmodel",sim_vortex_body_primangulararaxisfrictionmodel,false},
    {"sim_vortex_body_secmangulararaxisfrictionmodel",sim_vortex_body_secmangulararaxisfrictionmodel,false},
    {"sim_vortex_body_normalmangulararaxisfrictionmodel",sim_vortex_body_normalmangulararaxisfrictionmodel,false},
    {"sim_vortex_body_bitcoded",sim_vortex_body_bitcoded,false},
    {"sim_vortex_body_autosleepsteplivethreshold",sim_vortex_body_autosleepsteplivethreshold,false},
    {"sim_vortex_body_materialuniqueid",sim_vortex_body_materialuniqueid,false},
    {"sim_vortex_body_pureshapesasconvex",sim_vortex_body_pureshapesasconvex,false},
    {"sim_vortex_body_convexshapesasrandom",sim_vortex_body_convexshapesasrandom,false},
    {"sim_vortex_body_randomshapesasterrain",sim_vortex_body_randomshapesasterrain,false},
    {"sim_vortex_body_fastmoving",sim_vortex_body_fastmoving,false},
    {"sim_vortex_body_autoslip",sim_vortex_body_autoslip,false},
    {"sim_vortex_body_seclinaxissameasprimlinaxis",sim_vortex_body_seclinaxissameasprimlinaxis,false},
    {"sim_vortex_body_secangaxissameasprimangaxis",sim_vortex_body_secangaxissameasprimangaxis,false},
    {"sim_vortex_body_normangaxissameasprimangaxis",sim_vortex_body_normangaxissameasprimangaxis,false},
    {"sim_vortex_body_autoangulardamping",sim_vortex_body_autoangulardamping,false},
    {"sim_newton_global_stepsize",sim_newton_global_stepsize,false},
    {"sim_newton_global_contactmergetolerance",sim_newton_global_contactmergetolerance,false},
    {"sim_newton_global_constraintsolvingiterations",sim_newton_global_constraintsolvingiterations,false},
    {"sim_newton_global_bitcoded",sim_newton_global_bitcoded,false},
    {"sim_newton_global_multithreading",sim_newton_global_multithreading,false},
    {"sim_newton_global_exactsolver",sim_newton_global_exactsolver,false},
    {"sim_newton_global_highjointaccuracy",sim_newton_global_highjointaccuracy,false},
    {"sim_newton_joint_dependencyfactor",sim_newton_joint_dependencyfactor,false},
    {"sim_newton_joint_dependencyoffset",sim_newton_joint_dependencyoffset,false},
    {"sim_newton_joint_objectid",sim_newton_joint_objectid,false},
    {"sim_newton_joint_dependentobjectid",sim_newton_joint_dependentobjectid,false},
    {"sim_newton_body_staticfriction",sim_newton_body_staticfriction,false},
    {"sim_newton_body_kineticfriction",sim_newton_body_kineticfriction,false},
    {"sim_newton_body_restitution",sim_newton_body_restitution,false},
    {"sim_newton_body_lineardrag",sim_newton_body_lineardrag,false},
    {"sim_newton_body_angulardrag",sim_newton_body_angulardrag,false},
    {"sim_newton_body_bitcoded",sim_newton_body_bitcoded,false},
    {"sim_newton_body_fastmoving",sim_newton_body_fastmoving,false},
    {"sim_vortex_bodyfrictionmodel_box",sim_vortex_bodyfrictionmodel_box,false},
    {"sim_vortex_bodyfrictionmodel_scaledbox",sim_vortex_bodyfrictionmodel_scaledbox,false},
    {"sim_vortex_bodyfrictionmodel_proplow",sim_vortex_bodyfrictionmodel_proplow,false},
    {"sim_vortex_bodyfrictionmodel_prophigh",sim_vortex_bodyfrictionmodel_prophigh,false},
    {"sim_vortex_bodyfrictionmodel_scaledboxfast",sim_vortex_bodyfrictionmodel_scaledboxfast,false},
    {"sim_vortex_bodyfrictionmodel_neutral",sim_vortex_bodyfrictionmodel_neutral,false},
    {"sim_vortex_bodyfrictionmodel_none",sim_vortex_bodyfrictionmodel_none,false},
    {"sim_bullet_constraintsolvertype_sequentialimpulse",sim_bullet_constraintsolvertype_sequentialimpulse,false},
    {"sim_bullet_constraintsolvertype_nncg",sim_bullet_constraintsolvertype_nncg,false},
    {"sim_bullet_constraintsolvertype_dantzig",sim_bullet_constraintsolvertype_dantzig,false},
    {"sim_bullet_constraintsolvertype_projectedgaussseidel",sim_bullet_constraintsolvertype_projectedgaussseidel,false},
    {"sim_filtercomponent_originalimage",sim_filtercomponent_originalimage_deprecated,false},
    {"sim_filtercomponent_originaldepth",sim_filtercomponent_originaldepth_deprecated,false},
    {"sim_filtercomponent_uniformimage",sim_filtercomponent_uniformimage_deprecated,false},
    {"sim_filtercomponent_tooutput",sim_filtercomponent_tooutput_deprecated,false},
    {"sim_filtercomponent_tobuffer1",sim_filtercomponent_tobuffer1_deprecated,false},
    {"sim_filtercomponent_tobuffer2",sim_filtercomponent_tobuffer2_deprecated,false},
    {"sim_filtercomponent_frombuffer1",sim_filtercomponent_frombuffer1_deprecated,false},
    {"sim_filtercomponent_frombuffer2",sim_filtercomponent_frombuffer2_deprecated,false},
    {"sim_filtercomponent_swapbuffers",sim_filtercomponent_swapbuffers_deprecated,false},
    {"sim_filtercomponent_addbuffer1",sim_filtercomponent_addbuffer1_deprecated,false},
    {"sim_filtercomponent_subtractbuffer1",sim_filtercomponent_subtractbuffer1_deprecated,false},
    {"sim_filtercomponent_multiplywithbuffer1",sim_filtercomponent_multiplywithbuffer1_deprecated,false},
    {"sim_filtercomponent_horizontalflip",sim_filtercomponent_horizontalflip_deprecated,false},
    {"sim_filtercomponent_verticalflip",sim_filtercomponent_verticalflip_deprecated,false},
    {"sim_filtercomponent_rotate",sim_filtercomponent_rotate_deprecated,false},
    {"sim_filtercomponent_shift",sim_filtercomponent_shift_deprecated,false},
    {"sim_filtercomponent_resize",sim_filtercomponent_resize_deprecated,false},
    {"sim_filtercomponent_3x3filter",sim_filtercomponent_3x3filter_deprecated,false},
    {"sim_filtercomponent_5x5filter",sim_filtercomponent_5x5filter_deprecated,false},
    {"sim_filtercomponent_sharpen",sim_filtercomponent_sharpen_deprecated,false},
    {"sim_filtercomponent_edge",sim_filtercomponent_edge_deprecated,false},
    {"sim_filtercomponent_rectangularcut",sim_filtercomponent_rectangularcut_deprecated,false},
    {"sim_filtercomponent_circularcut",sim_filtercomponent_circularcut_deprecated,false},
    {"sim_filtercomponent_normalize",sim_filtercomponent_normalize_deprecated,false},
    {"sim_filtercomponent_intensityscale",sim_filtercomponent_intensityscale_deprecated,false},
    {"sim_filtercomponent_keeporremovecolors",sim_filtercomponent_keeporremovecolors_deprecated,false},
    {"sim_filtercomponent_scaleandoffsetcolors",sim_filtercomponent_scaleandoffsetcolors_deprecated,false},
    {"sim_filtercomponent_binary",sim_filtercomponent_binary_deprecated,false},
    {"sim_filtercomponent_swapwithbuffer1",sim_filtercomponent_swapwithbuffer1_deprecated,false},
    {"sim_filtercomponent_addtobuffer1",sim_filtercomponent_addtobuffer1_deprecated,false},
    {"sim_filtercomponent_subtractfrombuffer1",sim_filtercomponent_subtractfrombuffer1_deprecated,false},
    {"sim_filtercomponent_correlationwithbuffer1",sim_filtercomponent_correlationwithbuffer1_deprecated,false},
    {"sim_filtercomponent_colorsegmentation",sim_filtercomponent_colorsegmentation_deprecated,false},
    {"sim_filtercomponent_blobextraction",sim_filtercomponent_blobextraction_deprecated,false},
    {"sim_filtercomponent_imagetocoord",sim_filtercomponent_imagetocoord_deprecated,false},
    {"sim_filtercomponent_pixelchange",sim_filtercomponent_pixelchange_deprecated,false},
    {"sim_filtercomponent_velodyne",sim_filtercomponent_velodyne_deprecated,false},
    {"sim_filtercomponent_todepthoutput",sim_filtercomponent_todepthoutput_deprecated,false},
    {"sim_filtercomponent_customized",sim_filtercomponent_customized_deprecated,false},
    {"sim_buffer_uint8",sim_buffer_uint8,false},
    {"sim_buffer_int8",sim_buffer_int8,false},
    {"sim_buffer_uint16",sim_buffer_uint16,false},
    {"sim_buffer_int16",sim_buffer_int16,false},
    {"sim_buffer_uint32",sim_buffer_uint32,false},
    {"sim_buffer_int32",sim_buffer_int32,false},
    {"sim_buffer_float",sim_buffer_float,false},
    {"sim_buffer_double",sim_buffer_double,false},
    {"sim_buffer_uint8rgb",sim_buffer_uint8rgb,false},
    {"sim_buffer_uint8bgr",sim_buffer_uint8bgr,false},
    {"sim_imgcomb_vertical",sim_imgcomb_vertical,false},
    {"sim_imgcomb_horizontal",sim_imgcomb_horizontal,false},
    {"sim_dynmat_default",sim_dynmat_default,false},
    {"sim_dynmat_highfriction",sim_dynmat_highfriction,false},
    {"sim_dynmat_lowfriction",sim_dynmat_lowfriction,false},
    {"sim_dynmat_nofriction",sim_dynmat_nofriction,false},
    {"sim_dynmat_reststackgrasp",sim_dynmat_reststackgrasp,false},
    {"sim_dynmat_foot",sim_dynmat_foot,false},
    {"sim_dynmat_wheel",sim_dynmat_wheel,false},
    {"sim_dynmat_gripper",sim_dynmat_gripper,false},
    {"sim_dynmat_floor",sim_dynmat_floor,false},
    // for backward compatibility:
    {"sim_pplanfloatparam_x_min",sim_pplanfloatparam_x_min,false},
    {"sim_pplanfloatparam_x_range",sim_pplanfloatparam_x_range,false},
    {"sim_pplanfloatparam_y_min",sim_pplanfloatparam_y_min,false},
    {"sim_pplanfloatparam_y_range",sim_pplanfloatparam_y_range,false},
    {"sim_pplanfloatparam_z_min",sim_pplanfloatparam_z_min,false},
    {"sim_pplanfloatparam_z_range",sim_pplanfloatparam_z_range,false},
    {"sim_pplanfloatparam_delta_min",sim_pplanfloatparam_delta_min,false},
    {"sim_pplanfloatparam_delta_range",sim_pplanfloatparam_delta_range,false},
    {"sim_ui_menu_title",sim_ui_menu_title,false},
    {"sim_ui_menu_minimize",sim_ui_menu_minimize,false},
    {"sim_ui_menu_close",sim_ui_menu_close,false},
    {"sim_api_errormessage_ignore",sim_api_errormessage_ignore,false},
    {"sim_api_errormessage_report",sim_api_errormessage_report,false},
    {"sim_api_errormessage_output",sim_api_errormessage_output,false},
    {"sim_ui_property_visible",sim_ui_property_visible,false},
    {"sim_ui_property_visibleduringsimulationonly",sim_ui_property_visibleduringsimulationonly,false},
    {"sim_ui_property_moveable",sim_ui_property_moveable,false},
    {"sim_ui_property_relativetoleftborder",sim_ui_property_relativetoleftborder,false},
    {"sim_ui_property_relativetotopborder",sim_ui_property_relativetotopborder,false},
    {"sim_ui_property_fixedwidthfont",sim_ui_property_fixedwidthfont,false},
    {"sim_ui_property_systemblock",sim_ui_property_systemblock,false},
    {"sim_ui_property_settocenter",sim_ui_property_settocenter,false},
    {"sim_ui_property_rolledup",sim_ui_property_rolledup,false},
    {"sim_ui_property_selectassociatedobject",sim_ui_property_selectassociatedobject,false},
    {"sim_ui_property_visiblewhenobjectselected",sim_ui_property_visiblewhenobjectselected,false},
    {"sim_ui_property_systemblockcanmovetofront",sim_ui_property_systemblockcanmovetofront,false},
    {"sim_ui_property_pauseactive",sim_ui_property_pauseactive,false},
    {"sim_2delement_menu_title",sim_ui_menu_title,false},
    {"sim_2delement_menu_minimize",sim_ui_menu_minimize,false},
    {"sim_2delement_menu_close",sim_ui_menu_close,false},
    {"sim_2delement_property_visible",sim_ui_property_visible,false},
    {"sim_2delement_property_visibleduringsimulationonly",sim_ui_property_visibleduringsimulationonly,false},
    {"sim_2delement_property_moveable",sim_ui_property_moveable,false},
    {"sim_2delement_property_relativetoleftborder",sim_ui_property_relativetoleftborder,false},
    {"sim_2delement_property_relativetotopborder",sim_ui_property_relativetotopborder,false},
    {"sim_2delement_property_fixedwidthfont",sim_ui_property_fixedwidthfont,false},
    {"sim_2delement_property_systemblock",sim_ui_property_systemblock,false},
    {"sim_2delement_property_settocenter",sim_ui_property_settocenter,false},
    {"sim_2delement_property_rolledup",sim_ui_property_rolledup,false},
    {"sim_2delement_property_selectassociatedobject",sim_ui_property_selectassociatedobject,false},
    {"sim_2delement_property_visiblewhenobjectselected",sim_ui_property_visiblewhenobjectselected,false},
    {"sim_pathproperty_invert_velocity",sim_pathproperty_invert_velocity_deprecated,false},
    {"sim_pathproperty_infinite_acceleration",sim_pathproperty_infinite_acceleration_deprecated,false},
    {"sim_pathproperty_auto_velocity_profile_translation",sim_pathproperty_auto_velocity_profile_translation_deprecated,false},
    {"sim_pathproperty_auto_velocity_profile_rotation",sim_pathproperty_auto_velocity_profile_rotation_deprecated,false},
    {"sim_pathproperty_endpoints_at_zero",sim_pathproperty_endpoints_at_zero_deprecated,false},
    {"sim_boolparam_joint_motion_handling_enabled",sim_boolparam_joint_motion_handling_enabled_deprecated,false},
    {"sim_boolparam_path_motion_handling_enabled",sim_boolparam_path_motion_handling_enabled_deprecated,false},
    {"sim_jointmode_motion",sim_jointmode_motion_deprecated,false},
    {"sim.syscb_aos_run",sim_syscb_aos_run_old,false},
    {"sim.addonscriptcall_run",sim_syscb_aos_run_old,false},
    {"sim_addonscriptcall_run",sim_syscb_aos_run_old,false},
    {"sim_navigation_camerafly",sim_navigation_camerafly_old,false},
    {"",-1,false}
};

/*
void insertSerialPortLeftOver(int portHandle,std::string leftOver)
{
    serialPortHandles.push_back(portHandle);
    serialPortLeftOverData.push_back(leftOver);
}

std::string retrieveSerialPortLeftOver(int portHandle)
{
    std::string leftOver;
    for (int i=0;i<int(serialPortHandles.size());i++)
    {
        if (serialPortHandles[i]==portHandle)
        {
            serialPortHandles.erase(serialPortHandles.begin()+i);
            leftOver=serialPortLeftOverData[i];
            serialPortLeftOverData.erase(serialPortLeftOverData.begin()+i);
            break;
        }
    }
    return(leftOver);
}

void clearSerialPortLeftOver(int portHandle)
{
    for (int i=0;i<int(serialPortHandles.size());i++)
    {
        if (serialPortHandles[i]==portHandle)
        {
            serialPortHandles.erase(serialPortHandles.begin()+i);
            serialPortLeftOverData.erase(serialPortLeftOverData.begin()+i);
            break;
        }
    }
}
*/
bool isObjectAssociatedWithThisThreadedChildScriptValid(luaWrap_lua_State* L)
{
    int id=getCurrentScriptHandle(L);
    CLuaScriptObject* script=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(id);
    if (script==nullptr)
        return(false);
    int h=script->getObjectIDThatScriptIsAttachedTo_child();
    return(h!=-1);
}

void pushCorrectTypeOntoLuaStack(luaWrap_lua_State* L,const std::string& buff)
{ // Pushes nil, false, true, number or string (in that order!!) onto the stack depending on the buff content!
    int t=getCorrectType(buff);
    if (t==0)
        luaWrap_lua_pushnil(L);
    if (t==1)
        luaWrap_lua_pushboolean(L,0);
    if (t==2)
        luaWrap_lua_pushboolean(L,1);
    if (t==3)
    {
        float floatVal;
        tt::getValidFloat(buff.c_str(),floatVal);
        luaWrap_lua_pushnumber(L,floatVal);
    }
    if (t==4)
        luaWrap_lua_pushlstring(L,buff.c_str(),buff.length());
}

int getCorrectType(const std::string& buff)
{ // returns 0=nil, 1=boolean false, 2=boolean true, 3=number or 4=string (in that order!!) depending on the buff content!
    if (buff.length()!=0)
    {
        if (buff.length()!=strlen(buff.c_str()))
            return(4); // We have embedded zeros, this has definitively to be a string:
    }
    if (strcmp(buff.c_str(),"nil")==0)
        return(0);
    if (strcmp(buff.c_str(),"false")==0)
        return(1);
    if (strcmp(buff.c_str(),"true")==0)
        return(2);
    float floatVal;
    if (tt::getValidFloat(buff.c_str(),floatVal))
        return(3);
    return(4);
}

void getScriptTree(luaWrap_lua_State* L,bool selfIncluded,std::vector<int>& scriptHandles)
{ // Returns all scripts that are built under the current one
    scriptHandles.clear();
    int currentScriptID=getCurrentScriptHandle(L);

    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(currentScriptID);
    if (it!=nullptr)
    {
        if (it->getScriptType()==sim_scripttype_mainscript)
        { // we have a main script here
            if (selfIncluded)
                scriptHandles.push_back(currentScriptID);
            for (size_t i=0;i<App::currentWorld->sceneObjects->getObjectCount();i++)
            {
                CSceneObject* q=App::currentWorld->sceneObjects->getObjectFromIndex(i);
                CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(q->getObjectHandle());
                if (lso!=nullptr)
                    scriptHandles.push_back(lso->getScriptID());
            }
        }

        if (it->getScriptType()==sim_scripttype_childscript)
        { // we have a child script
            CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(it->getObjectIDThatScriptIsAttachedTo_child());
            if (obj!=nullptr)
            { // should always pass
                if (selfIncluded)
                    scriptHandles.push_back(currentScriptID);

                std::vector<CSceneObject*> objList;
                obj->getAllObjectsRecursive(&objList,false);
                for (int i=0;i<int(objList.size());i++)
                {
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(objList[i]->getObjectHandle());
                    if (lso!=nullptr)
                        scriptHandles.push_back(lso->getScriptID());
                }
            }
        }

        if (it->getScriptType()==sim_scripttype_customizationscript)
        { // we have a customization script
            CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(it->getObjectIDThatScriptIsAttachedTo_customization());
            if (obj!=nullptr)
            { // should always pass
                if (selfIncluded)
                {
                    CLuaScriptObject* aScript=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(obj->getObjectHandle());
                    if (aScript!=nullptr)
                        scriptHandles.push_back(aScript->getScriptID());
                }

                std::vector<CSceneObject*> objList;
                obj->getAllObjectsRecursive(&objList,false);
                for (int i=0;i<int(objList.size());i++)
                {
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(objList[i]->getObjectHandle());
                    if (lso!=nullptr)
                    {
                        scriptHandles.push_back(lso->getScriptID());
                    }
                }
            }
        }
    }
}

void getScriptChain(luaWrap_lua_State* L,bool selfIncluded,bool mainIncluded,std::vector<int>& scriptHandles)
{ // Returns all script IDs that are parents (or grand-parents,grand-grand-parents, etc.) of the current one
    scriptHandles.clear();
    int currentScriptID=getCurrentScriptHandle(L);

    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(currentScriptID);

    if (it!=nullptr)
    {
        if (it->getScriptType()==sim_scripttype_mainscript)
        { // we have a main script here
            if (selfIncluded&&mainIncluded)
                scriptHandles.push_back(currentScriptID);
        }

        if (it->getScriptType()==sim_scripttype_childscript)
        { // we have a child script here
            CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(it->getObjectIDThatScriptIsAttachedTo_child());
            if (obj!=nullptr)
            {
                if (selfIncluded)
                    scriptHandles.push_back(currentScriptID);
                while (obj->getParent()!=nullptr)
                {
                    obj=obj->getParent();
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(obj->getObjectHandle());
                    if (lso!=nullptr)
                        scriptHandles.push_back(lso->getScriptID());
                }
                if (mainIncluded)
                {
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getMainScript();
                    if (lso!=nullptr)
                        scriptHandles.push_back(lso->getScriptID());
                }
            }
        }

        if (it->getScriptType()==sim_scripttype_customizationscript)
        { // we have a customization script here
            CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(it->getObjectIDThatScriptIsAttachedTo_customization());
            if (obj!=nullptr)
            {
                if (selfIncluded)
                {
                    CLuaScriptObject* aScript=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(obj->getObjectHandle());
                    if (aScript!=nullptr)
                        scriptHandles.push_back(aScript->getScriptID());
                }
                while (obj->getParent()!=nullptr)
                {
                    obj=obj->getParent();
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(obj->getObjectHandle());
                    if (lso!=nullptr)
                        scriptHandles.push_back(lso->getScriptID());
                }
                if (mainIncluded)
                {
                    CLuaScriptObject* lso=App::currentWorld->luaScriptContainer->getMainScript();
                    if (lso!=nullptr)
                        scriptHandles.push_back(lso->getScriptID());
                }
            }
        }
    }
}

std::string getAdditionalLuaSearchPath()
{
    std::string retVal;
    retVal+=App::folders->getExecutablePath();
    retVal+="/?.lua;";
    retVal+=App::folders->getExecutablePath();
    retVal+="/lua/?.lua;";
    retVal+=App::folders->getExecutablePath();
    retVal+="/bwf/?.lua";
    if (App::currentWorld->mainSettings->getScenePathAndName().compare("")!=0)
    {
        retVal+=";";
        retVal+=App::currentWorld->mainSettings->getScenePath();
        retVal+="/?.lua";
    }
    if (App::userSettings->additionalLuaPath.length()>0)
    {
        retVal+=";";
        retVal+=App::userSettings->additionalLuaPath;
        retVal+="/?.lua";
    }
    return(retVal);
}


luaWrap_lua_State* initializeNewLuaState(int scriptNameIndex,int debugLevel)
{
    luaWrap_lua_State* L=luaWrap_luaL_newstate();
    luaWrap_luaL_openlibs(L);
    luaWrap_luaL_dostring(L,"os.setlocale'C'");

    // --------------------------------------------
    // append some paths to the Lua path variable:
    luaWrap_lua_getglobal(L,"package");
    luaWrap_lua_getfield(L,-1,"path");
    std::string cur_path=luaWrap_lua_tostring(L,-1);
    cur_path+=";";
    cur_path+=getAdditionalLuaSearchPath().c_str();
    boost::replace_all(cur_path,"\\","/");
    luaWrap_lua_pop(L,1);
    luaWrap_lua_pushstring(L,cur_path.c_str());
    luaWrap_lua_setfield(L,-2,"path");
    luaWrap_lua_pop(L,1);
    // --------------------------------------------

    luaWrap_luaL_dostring(L,"sim=require('sim')");
    prepareNewLuaVariables_onlyRequire(L); // Here we only handle things like: simUI=require('simExtCustomUI')
    registerNewLuaFunctions(L); // Important to handle functions before variables, so that in the line below we can assign functions to new function names (e.g. simExtCustomUI_create=simUI.create)
    prepareNewLuaVariables_noRequire(L);

    setScriptNameIndex(L,scriptNameIndex);
    luaWrap_luaL_dostring(L,"__HIDDEN__.executeAfterLuaStateInit()"); // needed for various

    int hookMask=luaWrapGet_LUA_MASKCOUNT();
    if (debugLevel>=sim_scriptdebug_allcalls)
        hookMask|=luaWrapGet_LUA_MASKCALL()|luaWrapGet_LUA_MASKRET();
    luaWrap_lua_sethook(L,luaHookFunction,hookMask,100); // This instruction gets also called in luaHookFunction!!!!

    return(L);
}

void registerTableFunction(luaWrap_lua_State* L,char const* const tableName,char const* const functionName,luaWrap_lua_CFunction functionCallback)
{
#ifdef OLD_LUA51
    luaWrap_lua_getfield(L,luaWrapGet_LUA_GLOBALSINDEX(),tableName);
    if (!luaWrap_lua_istable(L,-1))
    { // we first need to create the table
        luaWrap_lua_createtable(L,0,1);
        luaWrap_lua_setfield(L,luaWrapGet_LUA_GLOBALSINDEX(),tableName);
        luaWrap_lua_pop(L,1);
        luaWrap_lua_getfield(L,luaWrapGet_LUA_GLOBALSINDEX(),tableName);
    }
    luaWrap_lua_pushstring(L,functionName);
    luaWrap_lua_pushcfunction(L,functionCallback);
    luaWrap_lua_settable(L,-3);
    luaWrap_lua_pop(L,1);
#else
    luaWrap_lua_rawgeti(L,luaWrapGet_LUA_REGISTRYINDEX(),luaWrapGet_LUA_RIDX_GLOBALS()); // table of globals
    luaWrap_lua_getfield(L,-1,tableName);
    if (!luaWrap_lua_istable(L,-1))
    { // we first need to create the table
        luaWrap_lua_createtable(L,0,1);
        luaWrap_lua_setfield(L,-3,tableName);
        luaWrap_lua_pop(L,1);
        luaWrap_lua_getfield(L,-1,tableName);
    }
    luaWrap_lua_pushstring(L,functionName);
    luaWrap_lua_pushcfunction(L,functionCallback);
    luaWrap_lua_settable(L,-3);
    luaWrap_lua_pop(L,1);
    luaWrap_lua_pop(L,1); // pop table of globals
#endif
}

void registerNewLuaFunctions(luaWrap_lua_State* L)
{
    // CoppeliaSim API functions:
    for (int i=0;simLuaCommands[i].name!="";i++)
    {
        std::string name(simLuaCommands[i].name);
        if (name.find("sim.")!=std::string::npos)
        { // this is the new notation, e.g. sim.getObjectName()
            name.erase(name.begin(),name.begin()+4);
            registerTableFunction(L,"sim",name.c_str(),simLuaCommands[i].func);
        }
        else
            luaWrap_lua_register(L,simLuaCommands[i].name.c_str(),simLuaCommands[i].func);
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (int i=0;simLuaCommandsOldApi[i].name!="";i++)
        {
            std::string name(simLuaCommandsOldApi[i].name);
            if (name.find("sim.")!=std::string::npos)
            { // this is the new notation, e.g. sim.getObjectName()
                name.erase(name.begin(),name.begin()+4);
                registerTableFunction(L,"sim",name.c_str(),simLuaCommandsOldApi[i].func);
            }
            else
                luaWrap_lua_register(L,simLuaCommandsOldApi[i].name.c_str(),simLuaCommandsOldApi[i].func);
        }
    }
    // Plugin API functions:
    App::worldContainer->luaCustomFuncAndVarContainer->registerCustomLuaFunctions(L,_simGenericFunctionHandler);
}

void luaHookFunction(luaWrap_lua_State* L,luaWrap_lua_Debug* ar)
{
    TRACE_INTERNAL;
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
    if (it==nullptr)
        return; // the script ID was not yet set
    int debugLevel=it->getDebugLevel();

    if ( (ar->event==luaWrapGet_LUA_HOOKCALL())||(ar->event==luaWrapGet_LUA_HOOKRET()) )
    { // Debug call and return hooks
        if (debugLevel>=sim_scriptdebug_allcalls)
        { // debugging (further down also several occurences (auto thread switches and script end force)
            bool callIn=(ar->event==luaWrapGet_LUA_HOOKCALL());
            luaWrap_lua_getinfo(L, "nS", ar);
            it->handleDebug(ar->name,ar->what,callIn,false);
        }
    }

    if (ar->event!=luaWrapGet_LUA_HOOKCALL())
    { // Return and Count hook (return somehow needed here too when debug is on)
        // Following 6 instructions are important: it can happen that the user locks/unlocks automatic thread switch in a loop,
        // and that the hook function by malchance only gets called when the thread switches are not allowed (due to the loop
        // timing and hook call timing overlap) --> this thread doesn't switch and stays in a lua loop forever.
        // To avoid this we add some random component to the hook timing:
        int randComponent=rand()/(RAND_MAX/10);
        int hookMask=luaWrapGet_LUA_MASKCOUNT();
        if (debugLevel>=sim_scriptdebug_allcalls)
            hookMask|=luaWrapGet_LUA_MASKCALL()|luaWrapGet_LUA_MASKRET();
        luaWrap_lua_sethook(L,luaHookFunction,hookMask,95+randComponent);
        // Also remember: the hook gets also called when calling luaWrap_luaL_doString from c++ and similar!!

        int scriptType=it->getScriptType();
        if ( (scriptType==sim_scripttype_mainscript)||(scriptType==sim_scripttype_childscript) )
        {
#ifdef SIM_WITH_GUI
            if (App::userSettings->getAbortScriptExecutionTiming()!=0)
            {
                if ( (App::currentWorld->luaScriptContainer->getMainScriptExecTimeInMs()>(App::userSettings->getAbortScriptExecutionTiming()*1000))&&App::currentWorld->luaScriptContainer->getInMainScriptNow() )
                    App::currentWorld->simulation->showAndHandleEmergencyStopButton(true,it->getShortDescriptiveName().c_str());
            }

            if ( CThreadPool::getSimulationEmergencyStop() ) // No automatic yield when flagged for destruction!! ||it->getFlaggedForDestruction() )
            { // This is the only way a non-threaded script can yield. But threaded child scripts can also yield here
                it->terminateScriptExecutionExternally(true);
                return;
            }
#endif
            if (CThreadPool::getSimulationStopRequestedAndActivated())
            { // returns true only after 1-2 seconds after the request arrived
                if (!VThread::isCurrentThreadTheMainSimulationThread())
                { // Here only threaded scripts can yield!
                    it->terminateScriptExecutionExternally(false);
                    return;
                }
            }

            if (!VThread::isCurrentThreadTheMainSimulationThread())
            {
                if (CThreadPool::isSwitchBackToPreviousThreadNeeded())
                {
                    if (debugLevel!=sim_scriptdebug_none)
                        it->handleDebug("thread_automatic_switch","C",true,true);
                    CThreadPool::switchBackToPreviousThreadIfNeeded();
                    if (debugLevel!=sim_scriptdebug_none)
                        it->handleDebug("thread_automatic_switch","C",false,true);
                }
            }
        }
        else
        { // non-simulation scripts (i.e. add-ons and customization scripts)
#ifdef SIM_WITH_GUI
            if (App::userSettings->getAbortScriptExecutionTiming()!=0)
            {
                if ( it->getScriptExecutionTimeInMs()>(App::userSettings->getAbortScriptExecutionTiming()*1000) )
                {
                    App::currentWorld->simulation->showAndHandleEmergencyStopButton(true,it->getShortDescriptiveName().c_str());
                    if (CLuaScriptObject::emergencyStopButtonPressed)
                    {
                        CLuaScriptObject::emergencyStopButtonPressed=false;
                        it->terminateScriptExecutionExternally(true);
                    }
                }
                else
                    App::currentWorld->simulation->showAndHandleEmergencyStopButton(false,"");
            }
#endif
        }

#ifdef OLD_LUA51
        luaWrap_luaL_dostring(L,"return coroutine.running()");
        if (!luaWrap_lua_isnil(L,-1))
        {
            if (it->shouldAutoYield())
            {
                luaWrap_lua_pop(L,1);
                return luaWrap_lua_yield(L,0); // does a long jump and never returns
            }
        }
        luaWrap_lua_pop(L,1);
#else
        luaWrap_luaL_dostring(L,"return coroutine.isyieldable()");
        if (luaWrap_lua_toboolean(L,-1))
        {
            if (it->shouldAutoYield())
            {
                luaWrap_lua_pop(L,1);
                return luaWrap_lua_yield(L,0); // does a long jump and never returns
            }
        }
        luaWrap_lua_pop(L,1);
#endif
    }
}

void prepareNewLuaVariables_onlyRequire(luaWrap_lua_State* L)
{
    App::worldContainer->luaCustomFuncAndVarContainer->assignCustomVariables(L,true);
}

void prepareNewLuaVariables_noRequire(luaWrap_lua_State* L)
{
    for (int i=0;simLuaVariables[i].name!="";i++)
        setNewLuaVariable(L,simLuaVariables[i].name.c_str(),simLuaVariables[i].val);
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (int i=0;simLuaVariablesOldApi[i].name!="";i++)
            setNewLuaVariable(L,simLuaVariablesOldApi[i].name.c_str(),simLuaVariablesOldApi[i].val);
    }
    App::worldContainer->luaCustomFuncAndVarContainer->assignCustomVariables(L,false);
}

void setNewLuaVariable(luaWrap_lua_State* L,const char* name,int identifier)
{
    std::string tmp(name);
    tmp+="="+boost::lexical_cast<std::string>(identifier);
    luaWrap_luaL_dostring(L,tmp.c_str());
}

int getCurrentScriptHandle(luaWrap_lua_State* L)
{
    luaWrap_lua_getglobal(L,"sim_current_script_id");
    int retVal=-1;
    if (luaWrap_lua_isnumber(L,-1))
        retVal=luaWrap_lua_tointeger(L,-1);
    luaWrap_lua_pop(L,1);
    return(retVal);
}

void setScriptNameIndex(luaWrap_lua_State* L,int index)
{
    std::string tmp(SIM_SCRIPT_NAME_INDEX);
    tmp+="=";
    tmp+=std::to_string(index);
    luaWrap_luaL_dostring(L,tmp.c_str());
}

int getScriptNameIndex(luaWrap_lua_State* L)
{
    int retVal=-1;
    luaWrap_lua_getglobal(L,SIM_SCRIPT_NAME_INDEX);
    if (luaWrap_lua_isnumber(L,-1))
        retVal=luaWrap_lua_tointeger(L,-1);
    luaWrap_lua_pop(L,1);
    return(retVal);
}

bool readCustomFunctionDataFromStack(luaWrap_lua_State* L,int ind,int dataType,
                                     std::vector<char>& inBoolVector,
                                     std::vector<int>& inIntVector,
                                     std::vector<float>& inFloatVector,
                                     std::vector<double>& inDoubleVector,
                                     std::vector<std::string>& inStringVector,
                                     std::vector<std::string>& inCharVector,
                                     std::vector<int>& inInfoVector)
{ // return value false means there is no more data on the stack
    if (luaWrap_lua_gettop(L)<ind)
        return(false); // not enough data on the stack
    inInfoVector.push_back(sim_script_arg_invalid); // Dummy value for type
    inInfoVector.push_back(0);                      // dummy value for size
    if (luaWrap_lua_isnil(L,ind))
    { // Special case: nil should not generate a sim_script_arg_invalid type!
        inInfoVector[inInfoVector.size()-2]=sim_script_arg_null;
        return(true);
    }
    if (dataType&sim_script_arg_table)
    { // we have to read a table:
        dataType^=sim_script_arg_table;
        if (!luaWrap_lua_istable(L,ind))
            return(true); // this is not a table
        int dataSize=int(luaWrap_lua_rawlen(L,ind));
        std::vector<char> boolV;
        std::vector<int> intV;
        std::vector<float> floatV;
        std::vector<double> doubleV;
        std::vector<std::string> stringV;
        for (int i=0;i<dataSize;i++)
        {
            luaWrap_lua_rawgeti(L,ind,i+1);
            if (dataType==sim_script_arg_bool)
            {
                if (!luaWrap_lua_isboolean(L,-1))
                {
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                    return(true); // we don't have the correct data type
                }
                boolV.push_back(luaToBool(L,-1));
            }
            else if (dataType==sim_script_arg_int32)
            {
                if (!luaWrap_lua_isnumber(L,-1))
                {
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                    return(true); // we don't have the correct data type
                }
                intV.push_back(luaToInt(L,-1));
            }
            else if (dataType==sim_script_arg_float)
            {
                if (!luaWrap_lua_isnumber(L,-1))
                {
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                    return(true); // we don't have the correct data type
                }
                floatV.push_back(luaToFloat(L,-1));
            }
            else if (dataType==sim_script_arg_double)
            {
                if (!luaWrap_lua_isnumber(L,-1))
                {
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                    return(true); // we don't have the correct data type
                }
                doubleV.push_back(luaToDouble(L,-1));
            }
            else if (dataType==sim_script_arg_string)
            {
                if (!luaWrap_lua_isstring(L,-1))
                {
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                    return(true); // we don't have the correct data type
                }
                stringV.push_back(std::string(luaWrap_lua_tostring(L,-1)));
            }
            else
            {
                luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                return(true); // data type not recognized!
            }
            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
        }
        // All values in the tables passed the check!
        for (int i=0;i<int(boolV.size());i++)
            inBoolVector.push_back(boolV[i]);
        for (int i=0;i<int(intV.size());i++)
            inIntVector.push_back(intV[i]);
        for (int i=0;i<int(floatV.size());i++)
            inFloatVector.push_back(floatV[i]);
        for (int i=0;i<int(doubleV.size());i++)
            inDoubleVector.push_back(doubleV[i]);
        for (int i=0;i<int(stringV.size());i++)
            inStringVector.push_back(stringV[i]);
        inInfoVector[inInfoVector.size()-1]=dataSize; // Size of the table
        inInfoVector[inInfoVector.size()-2]=dataType|sim_script_arg_table; // Type
        return(true);
    }
    else
    { // we have simple data here (not a table)
        if (dataType==sim_script_arg_bool)
        {
            if (!luaWrap_lua_isboolean(L,ind))
                return(true); // we don't have the correct data type
            inBoolVector.push_back(luaToBool(L,ind));
            inInfoVector[inInfoVector.size()-2]=dataType;
        }
        else if (dataType==sim_script_arg_int32)
        {
            if (!luaWrap_lua_isnumber(L,ind))
                return(true); // we don't have the correct data type
            inIntVector.push_back(luaToInt(L,ind));
            inInfoVector[inInfoVector.size()-2]=dataType;
        }
        else if (dataType==sim_script_arg_float)
        {
            if (!luaWrap_lua_isnumber(L,ind))
                return(true); // we don't have the correct data type
            inFloatVector.push_back(luaToFloat(L,ind));
            inInfoVector[inInfoVector.size()-2]=dataType;
        }
        else if (dataType==sim_script_arg_double)
        {
            if (!luaWrap_lua_isnumber(L,ind))
                return(true); // we don't have the correct data type
            inDoubleVector.push_back(luaToDouble(L,ind));
            inInfoVector[inInfoVector.size()-2]=dataType;
        }
        else if (dataType==sim_script_arg_string)
        {
            if (!luaWrap_lua_isstring(L,ind))
                return(true); // we don't have the correct data type
            inStringVector.push_back(std::string(luaWrap_lua_tostring(L,ind)));
            inInfoVector[inInfoVector.size()-2]=dataType;
        }
        else if (dataType==sim_script_arg_charbuff)
        {
            if (!luaWrap_lua_isstring(L,ind))
                return(true); // we don't have the correct data type
            size_t dataLength;
            char* data=(char*)luaWrap_lua_tolstring(L,ind,&dataLength);
            inCharVector.push_back(std::string(data,dataLength));
            inInfoVector[inInfoVector.size()-2]=dataType;
            inInfoVector[inInfoVector.size()-1]=int(dataLength);
        }
        return(true); // data type not recognized!
    }
    return(true);
}

void writeCustomFunctionDataOntoStack(luaWrap_lua_State* L,int dataType,int dataSize,
                                      unsigned char* boolData,int& boolDataPos,
                                      int* intData,int& intDataPos,
                                      float* floatData,int& floatDataPos,
                                      double* doubleData,int& doubleDataPos,
                                      char* stringData,int& stringDataPos,
                                      char* charData,int& charDataPos)
{
    if (((dataType|sim_script_arg_table)-sim_script_arg_table)==sim_script_arg_charbuff)
    { // special handling here
        luaWrap_lua_pushlstring(L,charData+charDataPos,dataSize);
        charDataPos+=dataSize;
    }
    else
    {
        int newTablePos=0;
        bool weHaveATable=false;
        if (dataType&sim_script_arg_table)
        { // we have a table
            luaWrap_lua_newtable(L);
            newTablePos=luaWrap_lua_gettop(L);
            dataType^=sim_script_arg_table;
            weHaveATable=true;
        }
        else
            dataSize=1;
        for (int i=0;i<dataSize;i++)
        {
            if (dataType==sim_script_arg_bool)
                luaWrap_lua_pushboolean(L,boolData[boolDataPos++]);
            else if (dataType==sim_script_arg_int32)
                luaWrap_lua_pushinteger(L,intData[intDataPos++]);
            else if (dataType==sim_script_arg_float)
                luaWrap_lua_pushnumber(L,floatData[floatDataPos++]);
            else if (dataType==sim_script_arg_double)
                luaWrap_lua_pushnumber(L,doubleData[doubleDataPos++]);
            else if (dataType==sim_script_arg_string)
            {
                luaWrap_lua_pushstring(L,stringData+stringDataPos);
                stringDataPos+=(int)strlen(stringData+stringDataPos)+1; // Thanks to Ulrich Schwesinger for noticing a bug here!
            }
            else
                luaWrap_lua_pushnil(L); // that is an error!

            if (weHaveATable) // that's when we have a table
                luaWrap_lua_rawseti(L,newTablePos,i+1);
        }
    }
}

void getFloatsFromTable(luaWrap_lua_State* L,int tablePos,int floatCount,float* arrayField)
{
    for (int i=0;i<floatCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        arrayField[i]=luaToFloat(L,-1);
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
}

void getDoublesFromTable(luaWrap_lua_State* L,int tablePos,int doubleCount,double* arrayField)
{
    for (int i=0;i<doubleCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        arrayField[i]=luaWrap_lua_tonumber(L,-1);
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
}

bool getIntsFromTable(luaWrap_lua_State* L,int tablePos,int intCount,int* arrayField)
{
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        if (!luaWrap_lua_isnumber(L,-1))
        {
            luaWrap_lua_pop(L,1); // we pop one element from the stack;
            return(false); // Not a number!!
        }
        arrayField[i]=luaToInt(L,-1);
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
    return(true);
}

bool getUIntsFromTable(luaWrap_lua_State* L,int tablePos,int intCount,unsigned int* arrayField)
{
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        if (!luaWrap_lua_isnumber(L,-1))
        {
            luaWrap_lua_pop(L,1); // we pop one element from the stack;
            return(false); // Not a number!!
        }
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,-1);
        if (na<0.0)
            na=0.0;
        arrayField[i]=(unsigned int)na;
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
    return(true);
}

bool getUCharsFromTable(luaWrap_lua_State* L,int tablePos,int intCount,unsigned char* arrayField)
{
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        if (!luaWrap_lua_isnumber(L,-1))
        {
            luaWrap_lua_pop(L,1); // we pop one element from the stack;
            return(false); // Not a number!!
        }
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,-1);
        if (na<0.0)
            na=0.0;
        arrayField[i]=(unsigned char)na;
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
    return(true);
}

void getBoolsFromTable(luaWrap_lua_State* L,int tablePos,int boolCount,char* arrayField)
{
    for (int i=0;i<boolCount;i++)
    {
        luaWrap_lua_rawgeti(L,tablePos,i+1);
        arrayField[i]=(char)luaWrap_lua_toboolean(L,-1);
        luaWrap_lua_pop(L,1); // we pop one element from the stack;
    }
}

void pushFloatTableOntoStack(luaWrap_lua_State* L,int floatCount,const float* arrayField)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (int i=0;i<floatCount;i++)
    {
        luaWrap_lua_pushnumber(L,arrayField[i]);
        luaWrap_lua_rawseti(L,newTablePos,i+1);
    }
}

void pushDoubleTableOntoStack(luaWrap_lua_State* L,int doubleCount,const double* arrayField)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (int i=0;i<doubleCount;i++)
    {
        luaWrap_lua_pushnumber(L,arrayField[i]);
        luaWrap_lua_rawseti(L,newTablePos,i+1);
    }
}

void pushIntTableOntoStack(luaWrap_lua_State* L,int intCount,const int* arrayField)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_pushinteger(L,arrayField[i]);
        luaWrap_lua_rawseti(L,newTablePos,i+1);
    }
}

void pushUIntTableOntoStack(luaWrap_lua_State* L,int intCount,const unsigned int* arrayField)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_pushinteger(L,arrayField[i]);
        luaWrap_lua_rawseti(L,newTablePos,i+1);
    }
}

void pushUCharTableOntoStack(luaWrap_lua_State* L,int intCount,const unsigned char* arrayField)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (int i=0;i<intCount;i++)
    {
        luaWrap_lua_pushinteger(L,arrayField[i]);
        luaWrap_lua_rawseti(L,newTablePos,i+1);
    }
}

void pushStringTableOntoStack(luaWrap_lua_State* L,const std::vector<std::string>& stringTable)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (size_t i=0;i<stringTable.size();i++)
    {
        luaWrap_lua_pushstring(L,stringTable[i].c_str());
        luaWrap_lua_rawseti(L,newTablePos,(int)i+1);
    }
}

void pushLStringTableOntoStack(luaWrap_lua_State* L,const std::vector<std::string>& stringTable)
{
    luaWrap_lua_newtable(L);
    int newTablePos=luaWrap_lua_gettop(L);
    for (size_t i=0;i<stringTable.size();i++)
    {
        luaWrap_lua_pushlstring(L,stringTable[i].c_str(),stringTable[i].size());
        luaWrap_lua_rawseti(L,newTablePos,(int)i+1);
    }
}

void insertFloatsIntoTableAlreadyOnStack(luaWrap_lua_State* L,int tablePos,int floatCount,const float* arrayField)
{
    for (int i=0;i<floatCount;i++)
    {
        luaWrap_lua_pushnumber(L,arrayField[i]);
        luaWrap_lua_rawseti(L,tablePos,i+1);
    }
}

int luaToInt(luaWrap_lua_State* L,int pos)
{
    return((int)luaWrap_lua_tointeger(L,pos));
}

float luaToFloat(luaWrap_lua_State* L,int pos)
{
    return((float)luaWrap_lua_tonumber(L,pos));
}

double luaToDouble(luaWrap_lua_State* L,int pos)
{
    return(luaWrap_lua_tonumber(L,pos));
}

bool luaToBool(luaWrap_lua_State* L,int pos)
{
    return(luaWrap_lua_toboolean(L,pos)!=0);
}

int checkOneGeneralInputArgument(luaWrap_lua_State* L,int index,
                           int type,int cnt_orZeroIfNotTable,bool optional,bool nilInsteadOfTypeAndCountAllowed,std::string* errStr)
{ // return -1 means error, 0 means data is missing, 1 means data is nil, 2 means data is ok
    // if cnt_orZeroIfNotTable is -1, we are expecting a table, which could also be empty
    // 1. We check if there is something on the stack at that position:
    if (luaWrap_lua_gettop(L)<index)
    { // That data is missing:
        if (optional)
            return(0);
        if (errStr!=nullptr)
            errStr->assign(SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS);
        return(-1);
    }
    // 2. We check if we have nil on the stack:
    if (luaWrap_lua_isnil(L,index))
    { // We have nil.
        // Did we expect a boolean? If yes, it is ok
        if ( (type==lua_arg_bool)&&(cnt_orZeroIfNotTable==0) )
            return(2);
        if ( (type==lua_arg_nil)&&(cnt_orZeroIfNotTable==0) )
            return(2);
        if (nilInsteadOfTypeAndCountAllowed)
            return(1);
        if (errStr!=nullptr)
            errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
        return(-1);
    }
    // 3. we check if we expect a table:
    if (cnt_orZeroIfNotTable!=0) // was >=1 until 18/2/2016
    { 
        // We check if we really have a table at that position:
        if (!luaWrap_lua_istable(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(-1);
        }
        // we check the table size:
        if (int(luaWrap_lua_rawlen(L,index))<cnt_orZeroIfNotTable)
        { // the size is not correct
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_TABLE_SIZE_IS_WRONG);
            return(-1);
        }
        else
        { // we have the correct size
            // Now we need to check each element individually for the correct type:
            for (int i=0;i<cnt_orZeroIfNotTable;i++)
            {
                luaWrap_lua_rawgeti(L,index,i+1);
                if (!checkOneInputArgument(L,-1,type,errStr))
                    return(-1);
                luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
            }
            // Everything went fine:
            return(2);
        }
    }
    else
    { // we expect a non-table type
        if (checkOneInputArgument(L,index,type,errStr))
            return(2);
        return(-1);
    }
}

bool checkOneInputArgument(luaWrap_lua_State* L,int index,int type,std::string* errStr)
{
    // 1. We check if there is something on the stack at that position:
    if (luaWrap_lua_gettop(L)<index)
    { // That data is missing:
        if (errStr!=nullptr)
            errStr->assign(SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS);
        return(false);
    }
    if (type==lua_arg_number)
    {
        if (!luaWrap_lua_isnumber(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false); // error
        }
        return(true);
    }
    if (type==lua_arg_bool)
    { // since anything can be a bool value, we don't generate any error!
        return(true);
    }
    if (type==lua_arg_nil)
    { // Here we expect a nil value:
        if (!luaWrap_lua_isnil(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false);
        }
        return(true);
    }
    if (type==lua_arg_string)
    {
        if (!luaWrap_lua_isstring(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false); // error
        }
        return(true);
    }
    if (type==lua_arg_table)
    {
        if (!luaWrap_lua_istable(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false); // error
        }
        return(true);
    }
    if (type==lua_arg_function)
    {
        if (!luaWrap_lua_isfunction(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false); // error
        }
        return(true);
    }
    if (type==lua_arg_userdata)
    {
        if (!luaWrap_lua_isuserdata(L,index))
        {
            if (errStr!=nullptr)
                errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
            return(false); // error
        }
        return(true);
    }
    // Here we have a table
    if (errStr!=nullptr)
        errStr->assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
    return(false);
}

bool checkInputArguments(luaWrap_lua_State* L,std::string* errStr,
                         int type1,int type1Cnt_zeroIfNotTable,
                         int type2,int type2Cnt_zeroIfNotTable,
                         int type3,int type3Cnt_zeroIfNotTable,
                         int type4,int type4Cnt_zeroIfNotTable,
                         int type5,int type5Cnt_zeroIfNotTable,
                         int type6,int type6Cnt_zeroIfNotTable,
                         int type7,int type7Cnt_zeroIfNotTable,
                         int type8,int type8Cnt_zeroIfNotTable,
                         int type9,int type9Cnt_zeroIfNotTable,
                         int type10,int type10Cnt_zeroIfNotTable,
                         int type11,int type11Cnt_zeroIfNotTable)
{ // all arguments, except L and the functionName have default values
    // CAREFUL!!! if typeXCnt_zeroIfNotTable is 0, it means it is a simple value (not table)
    // if typeXCnt_zeroIfNotTable is >=1 then we are expecting a table with at least typeXCnt_zeroIfNotTable elements!!
    // if typeXCnt_zeroIfNotTable is =-1 then we are expecting a table that can have any number of elements
    int inArgs[11]={type1,type2,type3,type4,type5,type6,type7,type8,type9,type10,type11};
    int inArgsCnt_zeroIfNotTable[11]={type1Cnt_zeroIfNotTable,type2Cnt_zeroIfNotTable,type3Cnt_zeroIfNotTable,type4Cnt_zeroIfNotTable,type5Cnt_zeroIfNotTable,type6Cnt_zeroIfNotTable,type7Cnt_zeroIfNotTable,type8Cnt_zeroIfNotTable,type9Cnt_zeroIfNotTable,type10Cnt_zeroIfNotTable,type11Cnt_zeroIfNotTable};
    int totArgs=0;
    for (int i=0;i<11;i++)
    {
        if (inArgs[i]==lua_arg_empty)
            break;
        if (inArgsCnt_zeroIfNotTable[i]<-1)
        {
            App::logMsg(sim_verbosity_errors,"error in call to 'checkInputArguments' routine.");
            App::beep();
            while (true);
        }
        totArgs++;
    }
    int numberOfArguments=luaWrap_lua_gettop(L);
    if (numberOfArguments<totArgs)
    { // we don't have enough arguments!
        if (errStr!=nullptr)
            errStr->assign(SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS);
        return(false); // error
    }
    for (int i=0;i<totArgs;i++)
    {
//        if (checkOneGeneralInputArgument(L,i+1,inArgs[i],inArgsCnt_zeroIfNotTable[i],false,false,errStr)==-1)
        if (checkOneGeneralInputArgument(L,i+1,inArgs[i],inArgsCnt_zeroIfNotTable[i],false,false,errStr)!=2)
            return(false); // error
    }
    return(true);
}

bool doesEntityExist(luaWrap_lua_State* L,std::string* errStr,int identifier)
{
    if (identifier>=SIM_IDSTART_COLLECTION)
    {
        if (App::currentWorld->collections->getObjectFromHandle(identifier)==nullptr)
        {
            if (errStr!=nullptr)
                errStr[0]=SIM_ERROR_ENTITY_INEXISTANT;
            return(false);
        }
        return(true);
    }
    else
    {
        if (App::currentWorld->sceneObjects->getObjectFromHandle(identifier)==nullptr)
        {
            if (errStr!=nullptr)
                errStr[0]=SIM_ERROR_ENTITY_INEXISTANT;
            return(false);
        }
        return(true);
    }
}

int _genericFunctionHandler_new(luaWrap_lua_State* L,CLuaCustomFunction* func,std::string& raiseErrorWithMsg)
{
    TRACE_LUA_API;
    CInterfaceStack* stack=new CInterfaceStack();
    int stackId=App::worldContainer->interfaceStackContainer->addStack(stack);
    stack->buildFromLuaStack(L);

    // Now we retrieve the object ID this script might be attached to:
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    int linkedObject=-1;
    if (itObj->getScriptType()==sim_scripttype_childscript)
    {
        CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(itObj->getObjectIDThatScriptIsAttachedTo_child());
        if (obj!=nullptr)
            linkedObject=obj->getObjectHandle();
    }
    if (itObj->getScriptType()==sim_scripttype_customizationscript)
    {
        CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(itObj->getObjectIDThatScriptIsAttachedTo_customization());
        if (obj!=nullptr)
            linkedObject=obj->getObjectHandle();
    }

    // We prepare the callback structure:
    char raiseErrorMsg[258];
    raiseErrorMsg[0]='\0';
    SScriptCallBack* cb=new SScriptCallBack;
    cb->objectID=linkedObject;
    cb->scriptID=currentScriptID;
    cb->stackID=stackId;
    cb->waitUntilZero=0;
    cb->raiseErrorWithMessage=(char*)&raiseErrorMsg;

    // Now we can call the callback:
    func->callBackFunction_new(cb);

    bool dontDeleteStructureYet=false;
    while (cb->waitUntilZero!=0)
    { // backward compatibility (for real threads)
        if (!CThreadPool::switchBackToPreviousThread())
            break;
        if (CThreadPool::getSimulationStopRequestedAndActivated())
        { // give a chance to the c app to set the waitUntilZero to zero! (above turns true only 1-2 secs after the stop request arrived)
            // Following: the extension module might still write 0 into that position to signal "no more waiting" in
            // case this while loop got interrupted by a stop request.
            dontDeleteStructureYet=true;
            break;
        }
    }
    /*
    if (func->getFunctionName().compare("simUI.getPosition")==0)
    {
        std::string buff;
        stack->printContent(-1,buff);
        printf("%s\n",buff.c_str());
    }
    */
    // Now we have to build the returned data onto the stack:
    stack->buildOntoLuaStack(L,false);

    if (strlen(cb->raiseErrorWithMessage)!=0)
        raiseErrorWithMsg+=cb->raiseErrorWithMessage; // is this mechanism used?! We probably simply use simSetLastError..?

    // And we return the number of arguments:
    int outputArgCount=stack->getStackSize();
    if (dontDeleteStructureYet)
    {   // We cannot yet delete the structure because an extension module might still write '0' into
        // p->waitUntilZero!! We delete the structure at the end of the simulation.
        App::currentWorld->luaScriptContainer->addCallbackStructureObjectToDestroyAtTheEndOfSimulation_new(cb);
    }
    else
        delete cb;
    App::worldContainer->interfaceStackContainer->destroyStack(stackId);
    return(outputArgCount);
}

void appendAllSimFunctionNames_spaceSeparated(std::string& keywords,int scriptType,bool scriptIsThreaded)
{
    for (size_t i=0;simLuaCommands[i].name!="";i++)
    {
        keywords+=simLuaCommands[i].name;
        keywords+=" ";
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaCommandsOldApi[i].name!="";i++)
        {
            keywords+=simLuaCommandsOldApi[i].name;
            keywords+=" ";
        }
    }
    std::vector<std::string> sysCb=CLuaScriptObject::getAllSystemCallbackStrings(scriptType,scriptIsThreaded,false);
    for (size_t i=0;i<sysCb.size();i++)
    {
        keywords+=sysCb[i];
        keywords+=" ";
    }
}

void appendAllSimVariableNames_spaceSeparated(std::string& keywords)
{
    for (size_t i=0;simLuaVariables[i].name!="";i++)
    {
        keywords+=simLuaVariables[i].name;
        keywords+=" ";
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaVariablesOldApi[i].name!="";i++)
        {
            keywords+=simLuaVariablesOldApi[i].name;
            keywords+=" ";
        }
    }
}

void pushAllSimFunctionNamesThatStartSame_autoCompletionList(const char* txt,std::vector<std::string>& v,std::map<std::string,bool>& m,int scriptType,bool scriptIsThreaded)
{
    std::string ttxt(txt);
    bool hasDot=(ttxt.find('.')!=std::string::npos);
    for (size_t i=0;simLuaCommands[i].name!="";i++)
    {
        if (simLuaCommands[i].autoComplete)
        {
            std::string n(simLuaCommands[i].name);
            if ((n.size()>=ttxt.size())&&(n.compare(0,ttxt.size(),ttxt)==0))
            {
                if (!hasDot)
                {
                    size_t dp=n.find('.');
                    if ( (dp!=std::string::npos)&&(ttxt.size()>0) )
                        n.erase(n.begin()+dp,n.end()); // we only push the text up to the dot, if txt is not empty
                }
                std::map<std::string,bool>::iterator it=m.find(n);
                if (it==m.end())
                {
                    m[n]=true;
                    v.push_back(n);
                }
            }
        }
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaCommandsOldApi[i].name!="";i++)
        {
            if (simLuaCommandsOldApi[i].autoComplete)
            {
                std::string n(simLuaCommandsOldApi[i].name);
                if ((n.size()>=ttxt.size())&&(n.compare(0,ttxt.size(),ttxt)==0))
                {
                    if (!hasDot)
                    {
                        size_t dp=n.find('.');
                        if ( (dp!=std::string::npos)&&(ttxt.size()>0) )
                            n.erase(n.begin()+dp,n.end()); // we only push the text up to the dot, if txt is not empty
                    }
                    std::map<std::string,bool>::iterator it=m.find(n);
                    if (it==m.end())
                    {
                        m[n]=true;
                        v.push_back(n);
                    }
                }
            }
        }
    }
    std::vector<std::string> sysCb=CLuaScriptObject::getAllSystemCallbackStrings(scriptType,scriptIsThreaded,false);
    for (size_t i=0;i<sysCb.size();i++)
    {
        std::string n(sysCb[i]);
        if ((n.size()>=ttxt.size())&&(n.compare(0,ttxt.size(),ttxt)==0))
        {
            if (!hasDot)
            {
                size_t dp=n.find('.');
                if ( (dp!=std::string::npos)&&(ttxt.size()>0) )
                    n.erase(n.begin()+dp,n.end()); // we only push the text up to the dot, if txt is not empty
            }
            std::map<std::string,bool>::iterator it=m.find(n);
            if (it==m.end())
            {
                m[n]=true;
                v.push_back(n);
            }
        }
    }
}

void pushAllSimVariableNamesThatStartSame_autoCompletionList(const char* txt,std::vector<std::string>& v,std::map<std::string,bool>& m)
{
    std::string ttxt(txt);
    bool hasDot=(ttxt.find('.')!=std::string::npos);
    for (size_t i=0;simLuaVariables[i].name!="";i++)
    {
        if (simLuaVariables[i].autoComplete)
        {
            std::string n(simLuaVariables[i].name);
            if ((n.size()>=ttxt.size())&&(n.compare(0,ttxt.size(),ttxt)==0))
            {
                if (!hasDot)
                {
                    size_t dp=n.find('.');
                    if ( (dp!=std::string::npos)&&(ttxt.size()>0) )
                        n.erase(n.begin()+dp,n.end()); // we only push the text up to the dot, if txt is not empty
                }
                std::map<std::string,bool>::iterator it=m.find(n);
                if (it==m.end())
                {
                    m[n]=true;
                    v.push_back(n);
                }
            }
        }
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaVariablesOldApi[i].name!="";i++)
        {
            if (simLuaVariablesOldApi[i].autoComplete)
            {
                std::string n(simLuaVariablesOldApi[i].name);
                if ((n.size()>=ttxt.size())&&(n.compare(0,ttxt.size(),ttxt)==0))
                {
                    if (!hasDot)
                    {
                        size_t dp=n.find('.');
                        if ( (dp!=std::string::npos)&&(ttxt.size()>0) )
                            n.erase(n.begin()+dp,n.end()); // we only push the text up to the dot, if txt is not empty
                    }
                    std::map<std::string,bool>::iterator it=m.find(n);
                    if (it==m.end())
                    {
                        m[n]=true;
                        v.push_back(n);
                    }
                }
            }
        }
    }
}

std::string getSimFunctionCalltip(const char* txt,int scriptType,bool scriptIsThreaded,bool forceDoNotSupportOldApi)
{
    for (size_t i=0;simLuaCommands[i].name!="";i++)
    {
        if (simLuaCommands[i].name.compare(txt)==0)
            return(simLuaCommands[i].callTip);
    }
    if (App::userSettings->getSupportOldApiNotation()&&(!forceDoNotSupportOldApi))
    {
        for (size_t i=0;simLuaCommandsOldApi[i].name!="";i++)
        {
            if (simLuaCommandsOldApi[i].name.compare(txt)==0)
                return(simLuaCommandsOldApi[i].callTip);
        }
    }
    std::vector<std::string> sysCb=CLuaScriptObject::getAllSystemCallbackStrings(scriptType,scriptIsThreaded,false);
    std::vector<std::string> sysCbCt=CLuaScriptObject::getAllSystemCallbackStrings(scriptType,scriptIsThreaded,true);
    for (size_t i=0;i<sysCb.size();i++)
    {
        if (sysCb[i].compare(txt)==0)
            return(sysCbCt[i]);
    }
    return("");
}

int isFuncOrConstDeprecated(const char* txt)
{
    // Functions:
    for (size_t i=0;simLuaCommands[i].name!="";i++)
    {
        std::string n(simLuaCommands[i].name);
        if (n.compare(txt)==0)
        {
            if (simLuaCommands[i].autoComplete)
                return(0);
            return(1);
        }
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaCommandsOldApi[i].name!="";i++)
        {
            std::string n(simLuaCommandsOldApi[i].name);
            if (n.compare(txt)==0)
            {
                if (simLuaCommandsOldApi[i].autoComplete)
                    return(0);
                return(1);
            }
        }
    }
    std::vector<std::string> sysCb=CLuaScriptObject::getAllSystemCallbackStrings(-1,false,false);
    for (size_t i=0;i<sysCb.size();i++)
    {
        std::string n(sysCb[i]);
        if (n.compare(txt)==0)
            return(0);
    }

    // Variables/Constants:
    for (size_t i=0;simLuaVariables[i].name!="";i++)
    {
        std::string n(simLuaVariables[i].name);
        if (n.compare(txt)==0)
        {
            if (simLuaVariables[i].autoComplete)
                return(0);
            return(1);
        }
    }
    if (App::userSettings->getSupportOldApiNotation())
    {
        for (size_t i=0;simLuaVariablesOldApi[i].name!="";i++)
        {
            std::string n(simLuaVariablesOldApi[i].name);
            if (n.compare(txt)==0)
            {
                if (simLuaVariablesOldApi[i].autoComplete)
                    return(0);
                return(1);
            }
        }
    }
    return(-1);
}

//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------









int _simGenericFunctionHandler(luaWrap_lua_State* L)
{   // THIS FUNCTION SHOULD NOT LOCK THE API (AT LEAST NOT WHILE CALLING THE CALLBACK!!) SINCE IT IS NOT DIRECTLY ACCESSING THE API!!!!
    TRACE_LUA_API;
    LUA_START("sim.genericFunctionHandler");

    luaWrap_lua_pushvalue(L,luaWrap_lua_upvalueindex(1));
    int id=luaWrap_lua_tointeger(L,-1)-1;
    luaWrap_lua_pop(L,1); // we have to pop the pushed value to get the original stack state
    int outputArgCount=0;
    for (size_t j=0;j<App::worldContainer->luaCustomFuncAndVarContainer->allCustomFunctions.size();j++)
    { // we now search for the callback to call:
        CLuaCustomFunction* it=App::worldContainer->luaCustomFuncAndVarContainer->allCustomFunctions[j];
        if (it->getFunctionID()==id)
        { // we have the right one! Now we need to prepare the input and output argument arrays:
            functionName=it->getFunctionName();
            App::logMsg(sim_verbosity_debug,(std::string("sim.genericFunctionHandler: ")+functionName).c_str());
            if (it->getPluginName().size()!=0)
            {
                functionName+="@simExt";
                functionName+=it->getPluginName();
            }
            else
                functionName+="@plugin";

            if (it->getUsesStackToExchangeData())
                outputArgCount=_genericFunctionHandler_new(L,it,errorString);
            else
                outputArgCount=_genericFunctionHandler_old(L,it);
            break;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(outputArgCount);
}

int _simHandleChildScripts(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleChildScripts");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int callType=luaWrap_lua_tointeger(L,1);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(currentScriptID);
        if (it!=nullptr)
        {
            if (it->getScriptType()==sim_scripttype_mainscript)
            { // only the main script can call this function
                CInterfaceStack inStack;
                inStack.buildFromLuaStack(L,2);
                int startT=VDateTime::getTimeInMs();
                retVal=App::currentWorld->luaScriptContainer->handleCascadedScriptExecution(sim_scripttype_childscript,callType,&inStack,nullptr,nullptr);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetScriptName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getScriptName");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int a=luaToInt(L,1);
        if (a==sim_handle_self)
            a=getCurrentScriptHandle(L);
        char* name=simGetScriptName_internal(a);
        if (name!=nullptr)
        {
            luaWrap_lua_pushstring(L,name);
            simReleaseBuffer_internal(name);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectAssociatedWithScript(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectAssociatedWithScript");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int a=luaToInt(L,1);
        if (a==sim_handle_self)
            a=getCurrentScriptHandle(L);
        retVal=simGetObjectAssociatedWithScript_internal(a);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetScriptAssociatedWithObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getScriptAssociatedWithObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetScriptAssociatedWithObject_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetCustomizationScriptAssociatedWithObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getCustomizationScriptAssociatedWithObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetCustomizationScriptAssociatedWithObject_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simOpenModule(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.openModule");

    moduleCommonPart(L,sim_message_eventcallback_moduleopen,&errorString);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(1);
}
int _simCloseModule(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.closeModule");

    moduleCommonPart(L,sim_message_eventcallback_moduleclose,&errorString);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(1);
}

int _simHandleModule(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START_NO_CSIDE_ERROR("sim.handleModule");
    bool sensingPart=false;
    int res=checkOneGeneralInputArgument(L,2,lua_arg_bool,0,true,false,&errorString);

    if ( (res==0)||(res==2) )
    {
        if (res==2)
            sensingPart=(luaWrap_lua_toboolean(L,2)!=0);
        if (sensingPart)
            moduleCommonPart(L,sim_message_eventcallback_modulehandleinsensingpart,&errorString);
        else
            moduleCommonPart(L,sim_message_eventcallback_modulehandle,&errorString);
    }
    else
        luaWrap_lua_pushinteger(L,-1);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(1);
}

void moduleCommonPart(luaWrap_lua_State* L,int action,std::string* errorString)
{
    TRACE_LUA_API;
    std::string functionName;
    if (action==sim_message_eventcallback_moduleopen)
        functionName="sim.openModule";
    if (action==sim_message_eventcallback_moduleclose)
        functionName="sim.closeModule";
    if ( (action==sim_message_eventcallback_modulehandle)||(action==sim_message_eventcallback_modulehandleinsensingpart) )
        functionName="sim.handleModule";
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (it->getScriptType()!=sim_scripttype_mainscript)
    {
        if (errorString!=nullptr)
            errorString->assign(SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT);
        luaWrap_lua_pushinteger(L,-1);
    }
    else
    {
        bool handleAll=false;
        if (luaWrap_lua_isnumber(L,1))
        { // We try to check whether we have sim_handle_all as a number:
            if (luaToInt(L,1)==sim_handle_all)
            {
                handleAll=true;
                void* retVal=CPluginContainer::sendEventCallbackMessageToAllPlugins(action,nullptr,nullptr,nullptr);
                delete[] ((char*)retVal);
                luaWrap_lua_pushinteger(L,1);
            }
        }
        if (!handleAll)
        {
            if (checkInputArguments(L,errorString,lua_arg_string,0))
            {
                std::string modName(luaWrap_lua_tostring(L,1));
                void* retVal=CPluginContainer::sendEventCallbackMessageToAllPlugins(action,nullptr,(char*)modName.c_str(),nullptr);
                delete[] ((char*)retVal);
                luaWrap_lua_pushinteger(L,1);
            }
            else
                luaWrap_lua_pushinteger(L,-1);
        }
    }
}

int _simBoolOr32(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.boolOr32");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<4294967295.9)&&(nb<4294967295.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            luaWrap_lua_pushinteger(L,(long long int)(a|b));
            LUA_END(1);
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simBoolAnd32(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.boolAnd32");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<4294967295.9)&&(nb<4294967295.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            luaWrap_lua_pushinteger(L,(long long int)(a&b));
            LUA_END(1);
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simBoolXor32(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.boolXor32");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<4294967295.9)&&(nb<4294967295.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            luaWrap_lua_pushinteger(L,(long long int)(a^b));
            LUA_END(1);
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simHandleDynamics(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleDynamics");

    int retVal=-1; // means error
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ( (itScrObj->getScriptType()==sim_scripttype_mainscript)||(itScrObj->getScriptType()==sim_scripttype_childscript) )
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
            retVal=simHandleDynamics_internal(luaToFloat(L,1));
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT_OR_CHILD_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleCollision(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleCollision");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objHandle=luaToInt(L,1);
        retVal=simHandleCollision_internal(objHandle);
        if ( (retVal>0)&&(objHandle>=0) )
        {
            int collObjHandles[2];
            CCollisionObject* it=App::currentWorld->collisions->getObjectFromHandle(objHandle);
            if (it!=nullptr)
            {
                it->readCollision(collObjHandles);
                luaWrap_lua_pushinteger(L,retVal);
                pushIntTableOntoStack(L,2,collObjHandles);
                LUA_END(2);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadCollision(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readCollision");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objHandle=luaToInt(L,1);
        retVal=simReadCollision_internal(objHandle);
        if (retVal>0)
        {
            int collObjHandles[2];
            CCollisionObject* it=App::currentWorld->collisions->getObjectFromHandle(objHandle);
            if (it!=nullptr)
            {
                it->readCollision(collObjHandles);
                luaWrap_lua_pushinteger(L,retVal);
                pushIntTableOntoStack(L,2,collObjHandles);
                LUA_END(2);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleDistance(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleDistance");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float d;
        retVal=simHandleDistance_internal(luaToInt(L,1),&d);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,d);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadDistance(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readDistance");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float d;
        retVal=simReadDistance_internal(luaToInt(L,1),&d);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,d);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleProximitySensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleProximitySensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float detPt[4];
        int detectedObjectID;
        float surfaceNormal[3];
        retVal=simHandleProximitySensor_internal(luaToInt(L,1),detPt,&detectedObjectID,surfaceNormal);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,detPt[3]);
            pushFloatTableOntoStack(L,3,detPt);
            luaWrap_lua_pushinteger(L,detectedObjectID);
            pushFloatTableOntoStack(L,3,surfaceNormal);
            LUA_END(5);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadProximitySensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readProximitySensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float detPt[4];
        int detectedObjectID;
        float surfaceNormal[3];
        retVal=simReadProximitySensor_internal(luaToInt(L,1),detPt,&detectedObjectID,surfaceNormal);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,detPt[3]);
            pushFloatTableOntoStack(L,3,detPt);
            luaWrap_lua_pushinteger(L,detectedObjectID);
            pushFloatTableOntoStack(L,3,surfaceNormal);
            LUA_END(5);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleVisionSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleVisionSensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float* auxVals=nullptr;
        int* auxValsCount=nullptr;
        retVal=simHandleVisionSensor_internal(luaToInt(L,1),&auxVals,&auxValsCount);
        if ((retVal!=-1)&&(auxValsCount!=nullptr))
        {
            int off=0;
            luaWrap_lua_pushinteger(L,retVal);
            int tableCount=auxValsCount[0];
            for (int i=0;i<tableCount;i++)
            {
                pushFloatTableOntoStack(L,auxValsCount[i+1],auxVals+off);
                off+=auxValsCount[i+1];
            }
            delete[] auxValsCount;
            delete[] auxVals;
            LUA_END(1+tableCount);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadVisionSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readVisionSensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float* auxVals=nullptr;
        int* auxValsCount=nullptr;
        retVal=simReadVisionSensor_internal(luaToInt(L,1),&auxVals,&auxValsCount);
        if ((retVal!=-1)&&(auxValsCount!=nullptr))
        {
            int off=0;
            luaWrap_lua_pushinteger(L,retVal);
            int tableCount=auxValsCount[0];
            for (int i=0;i<tableCount;i++)
            {
                pushFloatTableOntoStack(L,auxValsCount[i+1],auxVals+off);
                off+=auxValsCount[i+1];
            }
            delete[] auxValsCount;
            delete[] auxVals;
            LUA_END(1+tableCount);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetCollision(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetCollision");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetCollision_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetDistance(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetDistance");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetDistance_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetProximitySensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetProximitySensor");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetProximitySensor_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetVisionSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetVisionSensor");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetVisionSensor_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckProximitySensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkProximitySensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float detPt[4];
        retVal=simCheckProximitySensor_internal(luaToInt(L,1),luaToInt(L,2),detPt);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,detPt[3]);
            pushFloatTableOntoStack(L,3,detPt);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckProximitySensorEx(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkProximitySensorEx");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        float detPt[4];
        int detObj;
        float normVect[3];
        retVal=simCheckProximitySensorEx_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),luaToFloat(L,4),luaToFloat(L,5),detPt,&detObj,normVect);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,detPt[3]);
            pushFloatTableOntoStack(L,3,detPt);
            luaWrap_lua_pushinteger(L,detObj);
            pushFloatTableOntoStack(L,3,normVect);
            LUA_END(5);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckProximitySensorEx2(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkProximitySensorEx2");

    int retVal=-1; // means error
    if (checkOneGeneralInputArgument(L,1,lua_arg_number,0,false,false,&errorString)==2)
    { // first argument (sensor handle)
        int sensorID=luaToInt(L,1);
        if (checkOneGeneralInputArgument(L,3,lua_arg_number,0,false,false,&errorString)==2)
        { // third argument (item type)
            int itemType=luaToInt(L,3);
            if (checkOneGeneralInputArgument(L,4,lua_arg_number,0,false,false,&errorString)==2)
            { // forth argument (item count)
                int itemCount=luaToInt(L,4);
                int requiredValues=itemCount*3*(itemType+1);
                if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,requiredValues,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
                {
                    int mode=luaToInt(L,5);
                    float threshold=luaToFloat(L,6);
                    float maxAngle=luaToFloat(L,7);
                    float* vertices=new float[requiredValues];
                    getFloatsFromTable(L,2,requiredValues,vertices);

                    float detPt[4];
                    float normVect[3];
                    retVal=simCheckProximitySensorEx2_internal(sensorID,vertices,itemType,itemCount,mode,threshold,maxAngle,detPt,normVect);
                    delete[] vertices;
                    if (retVal==1)
                    {
                        luaWrap_lua_pushinteger(L,retVal);
                        luaWrap_lua_pushnumber(L,detPt[3]);
                        pushFloatTableOntoStack(L,3,detPt);
                        pushFloatTableOntoStack(L,3,normVect);
                        LUA_END(4);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetVisionSensorResolution(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getVisionSensorResolution");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int resolution[2];
        if (simGetVisionSensorResolution_internal(luaToInt(L,1),resolution)==1)
        {
            pushIntTableOntoStack(L,2,resolution);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetVisionSensorImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getVisionSensorImage");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int arg1=luaToInt(L,1);
        int handleFlags=arg1&0xff00000;
        int objectHandle=arg1&0xfffff;
        int valPerPix=3;
        int rgbOrGreyOrDepth=0;
        if ((handleFlags&sim_handleflag_greyscale)!=0)
        {
            valPerPix=1;
            rgbOrGreyOrDepth=1;
        }
        int posX=0;
        int posY=0;
        int sizeX=0;
        int sizeY=0;
        int retType=0;
        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                posX=luaToInt(L,2);
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    posY=luaToInt(L,3);
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        sizeX=luaToInt(L,4);
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                    if ((res==0)||(res==2))
                    {
                        if (res==2)
                            sizeY=luaToInt(L,5);
                        res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,false,&errorString);
                        if ((res==0)||(res==2))
                        {
                            if (res==2)
                                retType=luaToInt(L,6);
                            CVisionSensor* rs=App::currentWorld->sceneObjects->getVisionSensorFromHandle(objectHandle);
                            if (rs!=nullptr)
                            {
                                if ( (sizeX==0)&&(sizeY==0) )
                                { // we have default values here (the whole picture)
                                    int reso[2];
                                    rs->getRealResolution(reso);
                                    sizeX=reso[0];
                                    sizeY=reso[1];
                                }
                                float* buffer=rs->readPortionOfImage(posX,posY,sizeX,sizeY,rgbOrGreyOrDepth);
                                if (buffer!=nullptr)
                                {
                                    if (retType==0)
                                        pushFloatTableOntoStack(L,sizeX*sizeY*valPerPix,buffer);
                                    else
                                    { // here we return RGB data in a string
                                        char* str=new char[sizeX*sizeY*valPerPix];
                                        int vvv=sizeX*sizeY*valPerPix;
                                        for (int i=0;i<vvv;i++)
                                            str[i]=char(buffer[i]*255.0001f);
                                        luaWrap_lua_pushlstring(L,(const char*)str,vvv);
                                        delete[] ((char*)str);
                                    }
                                    delete[] ((char*)buffer);
                                    LUA_END(1);
                                }
                                else
                                    errorString=SIM_ERROR_INVALID_ARGUMENTS;
                            }
                            else
                                errorString=SIM_ERROR_VISION_SENSOR_INEXISTANT;
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetVisionSensorCharImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getVisionSensorCharImage");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int arg1=luaToInt(L,1);
        int handleFlags=arg1&0xff00000;
        int objectHandle=arg1&0xfffff;
        int valPerPix=3;
        if ((handleFlags&sim_handleflag_greyscale)!=0)
            valPerPix=1;
        int posX=0;
        int posY=0;
        int sizeX=0;
        int sizeY=0;
        float rgbaCutOff=0.0f;
        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                posX=luaToInt(L,2);
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    posY=luaToInt(L,3);
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        sizeX=luaToInt(L,4);
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                    if ((res==0)||(res==2))
                    {
                        if (res==2)
                            sizeY=luaToInt(L,5);
                        res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,false,&errorString);
                        if ((res==0)||(res==2))
                        {
                            if (res==2)
                                rgbaCutOff=luaToFloat(L,6);
                            CVisionSensor* rs=App::currentWorld->sceneObjects->getVisionSensorFromHandle(objectHandle);
                            if (rs!=nullptr)
                            {
                                int reso[2];
                                rs->getRealResolution(reso);
                                if ( (sizeX==0)&&(sizeY==0) )
                                { // we have default values here (the whole picture)
                                    sizeX=reso[0];
                                    sizeY=reso[1];
                                }
                                unsigned char* buffer=rs->readPortionOfCharImage(posX,posY,sizeX,sizeY,rgbaCutOff,valPerPix==1);
                                if (buffer!=nullptr)
                                {
                                    int vvv=sizeX*sizeY*valPerPix;
                                    if (rgbaCutOff>0.0f)
                                    {
                                        if (valPerPix==1)
                                            vvv=sizeX*sizeY*2;
                                        else
                                            vvv=sizeX*sizeY*4;
                                    }
                                    luaWrap_lua_pushlstring(L,(const char*)buffer,vvv);
                                    delete[] ((char*)buffer);
                                    luaWrap_lua_pushinteger(L,reso[0]);
                                    luaWrap_lua_pushinteger(L,reso[1]);
                                    LUA_END(3);
                                }
                                else
                                    errorString=SIM_ERROR_INVALID_ARGUMENTS;
                            }
                            else
                                errorString=SIM_ERROR_VISION_SENSOR_INEXISTANT;
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetVisionSensorImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setVisionSensorImage");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int arg1=luaToInt(L,1);
        int handleFlags=arg1&0xff00000;
        int sensHandle=arg1&0xfffff;
        int valPerPix=3;
        bool noProcessing=false;
        bool setDepthBufferInstead=false;
        if ((handleFlags&sim_handleflag_greyscale)!=0)
            valPerPix=1;
        if ((handleFlags&sim_handleflag_rawvalue)!=0)
            noProcessing=true;
        if ((handleFlags&sim_handleflag_depthbuffer)!=0)
            setDepthBufferInstead=true;
        CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(sensHandle);
        if (it!=nullptr)
        { // Ok we have a valid object
            if (it->getObjectType()==sim_object_visionsensor_type)
            { // ok we have a valid vision sensor
                int res[2];
                CVisionSensor* rendSens=(CVisionSensor*)it;
                rendSens->getRealResolution(res);
                // We check if we have a table or string at position 2:
                bool notTableNorString=true;
                if (luaWrap_lua_istable(L,2))
                { // Ok we have a table. Now what size is it?
                    notTableNorString=false;
                    if (setDepthBufferInstead)
                    {
                        // Now we check if the provided table has correct size:
                        if (int(luaWrap_lua_rawlen(L,2))>=res[0]*res[1])
                        {
                            float* img=new float[res[0]*res[1]];
                            getFloatsFromTable(L,2,res[0]*res[1],img);
                            rendSens->setDepthBuffer(img);
                            retVal=1;
                            delete[] img;
                        }
                        else
                            errorString=SIM_ERROR_ONE_TABLE_SIZE_IS_WRONG;
                    }
                    else
                    {
                        // Now we check if the provided table has correct size:
                        if (int(luaWrap_lua_rawlen(L,2))>=res[0]*res[1]*valPerPix)
                        {
                            float* img=new float[res[0]*res[1]*valPerPix];
                            getFloatsFromTable(L,2,res[0]*res[1]*valPerPix,img); // we do the operation directly without going through the c-api
                            if (rendSens->setExternalImage(img,valPerPix==1,noProcessing))
                                retVal=1;
                            delete[] img;
                        }
                        else
                            errorString=SIM_ERROR_ONE_TABLE_SIZE_IS_WRONG;
                    }
                }
                if (luaWrap_lua_isstring(L,2))
                { // Ok we have a string. Now what size is it?
                    notTableNorString=false;
                    // Now we check if the provided string has correct size:
                    size_t dataLength;
                    char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
                    if (setDepthBufferInstead)
                    {
                        if (int(dataLength)>=res[0]*res[1]*sizeof(float))
                        {
                            rendSens->setDepthBuffer((float*)data);
                            retVal=1;
                        }
                        else
                            errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
                    }
                    else
                    {
                        if (int(dataLength)>=res[0]*res[1]*valPerPix)
                        {
                            float* img=new float[res[0]*res[1]*valPerPix];
                            for (int i=0;i<res[0]*res[1]*valPerPix;i++)
                                img[i]=float(data[i])/255.0f;
                            if (rendSens->setExternalImage(img,valPerPix==1,noProcessing))
                                retVal=1;
                            delete[] img;
                        }
                        else
                            errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
                    }
                }
                if (notTableNorString)
                    errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
            }
            else
                errorString=SIM_ERROR_OBJECT_NOT_VISION_SENSOR;
        }
        else
            errorString=SIM_ERROR_OBJECT_INEXISTANT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetVisionSensorCharImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setVisionSensorCharImage");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int arg1=luaToInt(L,1);
        int handleFlags=arg1&0xff00000;
        int sensHandle=arg1&0xfffff;
        int valPerPix=3;
        if ((handleFlags&sim_handleflag_greyscale)!=0)
            valPerPix=1;
        bool noProcessing=false;
        if ((handleFlags&sim_handleflag_rawvalue)!=0)
            noProcessing=true;
        CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(sensHandle);
        if (it!=nullptr)
        { // Ok we have a valid object
            if (it->getObjectType()==sim_object_visionsensor_type)
            { // ok we have a valid vision sensor
                int res[2];
                CVisionSensor* rendSens=(CVisionSensor*)it;
                rendSens->getRealResolution(res);
                // We check if we have a string at position 2:
                if (luaWrap_lua_isstring(L,2))
                { // Ok we have a string. Now what size is it?
                    // Now we check if the provided string has correct size:
                    size_t dataLength;
                    char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
                    if (int(dataLength)>=res[0]*res[1]*valPerPix)
                    {
                        if (rendSens->setExternalCharImage((unsigned char*)data,valPerPix==1,noProcessing))
                            retVal=1;
                    }
                    else
                        errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
                }
                else
                    errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
            }
            else
                errorString=SIM_ERROR_OBJECT_NOT_VISION_SENSOR;
        }
        else
            errorString=SIM_ERROR_OBJECT_INEXISTANT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetVisionSensorDepthBuffer(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getVisionSensorDepthBuffer");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int posX=0;
        int posY=0;
        int sizeX=0;
        int sizeY=0;
        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                posX=luaToInt(L,2);
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    posY=luaToInt(L,3);
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        sizeX=luaToInt(L,4);
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                    if ((res==0)||(res==2))
                    {
                        if (res==2)
                            sizeY=luaToInt(L,5);
                        int sensHandle=luaToInt(L,1);
                        bool returnString=(sensHandle&sim_handleflag_codedstring)!=0;
                        bool toMeters=(sensHandle&sim_handleflag_depthbuffermeters)!=0;
                        sensHandle=sensHandle&0xfffff;
                        CVisionSensor* rs=App::currentWorld->sceneObjects->getVisionSensorFromHandle(sensHandle);
                        if (rs!=nullptr)
                        {
                            if ( (sizeX==0)&&(sizeY==0) )
                            { // we have default values here (the whole picture)
                                int reso[2];
                                rs->getRealResolution(reso);
                                sizeX=reso[0];
                                sizeY=reso[1];
                            }
                            float* buffer=rs->readPortionOfImage(posX,posY,sizeX,sizeY,2);
                            if (buffer!=nullptr)
                            {
                                if (toMeters)
                                { // Here we need to convert values to distances in meters:
                                    float n=rs->getNearClippingPlane();
                                    float f=rs->getFarClippingPlane();
                                    float fmn=f-n;
                                    for (int i=0;i<sizeX*sizeY;i++)
                                        buffer[i]=n+fmn*buffer[i];
                                }
                                if (returnString)
                                    luaWrap_lua_pushlstring(L,(char*)buffer,sizeX*sizeY*sizeof(float));
                                else
                                    pushFloatTableOntoStack(L,sizeX*sizeY,buffer);
                                delete[] ((char*)buffer);
                                LUA_END(1);
                            }
                            else
                                errorString=SIM_ERROR_INVALID_ARGUMENTS;
                        }
                        else
                            errorString=SIM_ERROR_VISION_SENSOR_INEXISTANT;
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCheckVisionSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkVisionSensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float* auxVals=nullptr;
        int* auxValsCount=nullptr;
        retVal=simCheckVisionSensor_internal(luaToInt(L,1),luaToInt(L,2),&auxVals,&auxValsCount);
        if ((retVal!=-1)&&(auxValsCount!=nullptr))
        {
            int off=0;
            luaWrap_lua_pushinteger(L,retVal);
            int tableCount=auxValsCount[0];
            for (int i=0;i<tableCount;i++)
            {
                pushFloatTableOntoStack(L,auxValsCount[i+1],auxVals+off);
                off+=auxValsCount[i+1];
            }
            delete[] auxValsCount;
            delete[] auxVals;
            LUA_END(1+tableCount);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckVisionSensorEx(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkVisionSensorEx");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_bool,0))
    {
        simBool returnImage=luaToBool(L,3);
        int arg1=luaToInt(L,1);
        int handleFlags=arg1&0xff00000;
        int sensHandle=arg1&0xfffff;
        int res[2];
        simGetVisionSensorResolution_internal(sensHandle,res);
        float* buffer=simCheckVisionSensorEx_internal(luaToInt(L,1),luaToInt(L,2),returnImage);
        if (buffer!=nullptr)
        {
            if ((handleFlags&sim_handleflag_codedstring)!=0)
            {
                if (returnImage)
                {
                    unsigned char* buff2=new unsigned char[res[0]*res[1]*3];
                    for (size_t i=0;i<res[0]*res[1]*3;i++)
                        buff2[i]=(unsigned char)(buffer[i]*255.1f);
                    luaWrap_lua_pushlstring(L,(const char*)buff2,res[0]*res[1]*3);
                    delete[] buff2;
                }
                else
                    luaWrap_lua_pushlstring(L,(const char*)buffer,res[0]*res[1]*sizeof(float));
            }
            else
            {
                if (returnImage)
                    pushFloatTableOntoStack(L,res[0]*res[1]*3,buffer);
                else
                    pushFloatTableOntoStack(L,res[0]*res[1],buffer);
            }
            simReleaseBuffer_internal((char*)buffer);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjects(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjects");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simGetObjects_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectHandle(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectHandle");

    int retVal=-1; // means error

    bool checkWithString=true;
    if (checkInputArguments(L,nullptr,lua_arg_number,0)) // do not output error if not string
    { // argument sim.handle_self
        if (luaWrap_lua_tointeger(L,1)==sim_handle_self)
        {
            checkWithString=false;
            int a=getCurrentScriptHandle(L);
            retVal=simGetObjectAssociatedWithScript_internal(a);
        }
    }
    if (checkWithString)
    {
        if (checkInputArguments(L,&errorString,lua_arg_string,0))
        {
            std::string name(luaWrap_lua_tostring(L,1));
            setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
            retVal=simGetObjectHandle_internal(name.c_str());
            setCurrentScriptNameIndex_cSide(-1);

        }
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddScript(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addScript");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int scriptType=luaToInt(L,1);
        retVal=simAddScript_internal(scriptType);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAssociateScriptWithObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.associateScriptWithObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int scriptHandle=luaToInt(L,1);
        int objectHandle=luaToInt(L,2);
        retVal=simAssociateScriptWithObject_internal(scriptHandle,objectHandle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetScriptText(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setScriptText");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int scriptHandle=luaToInt(L,1);
        std::string scriptText(luaWrap_lua_tostring(L,2));
        retVal=simSetScriptText_internal(scriptHandle,scriptText.c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simGetScriptHandle(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getScriptHandle");

    int retVal=-1; // means error
    if (luaWrap_lua_gettop(L)==0) // no arguments
        retVal=getCurrentScriptHandle(L);
    else
    {
        if (checkInputArguments(L,nullptr,lua_arg_nil,0)) // we don't output errors here!!
            retVal=getCurrentScriptHandle(L); // nil argument
        else
        {
            if (checkInputArguments(L,nullptr,lua_arg_number,0))
            { // argument sim.handle_self
                if (luaWrap_lua_tointeger(L,1)==sim_handle_self)
                    retVal=getCurrentScriptHandle(L);
            }
            if ( (retVal==-1)&&checkInputArguments(L,&errorString,lua_arg_string,0) )
            { // string argument
                std::string name(luaWrap_lua_tostring(L,1));
                setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
                retVal=simGetScriptHandle_internal(name.c_str());
                setCurrentScriptNameIndex_cSide(-1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetCollisionHandle(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getCollisionHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetCollisionHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveScript(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeScript");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
            handle=getCurrentScriptHandle(L);
        if (sim_handle_all!=handle)
            retVal=simRemoveScript_internal(handle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetDistanceHandle(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getDistanceHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetDistanceHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetCollectionHandle(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getCollectionHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetCollectionHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveCollection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeCollection");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simRemoveCollection_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simEmptyCollection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.emptyCollection");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simEmptyCollection_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectPosition");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float coord[3];
        if (simGetObjectPosition_internal(luaToInt(L,1),luaToInt(L,2),coord)==1)
        {
            pushFloatTableOntoStack(L,3,coord);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectOrientation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectOrientation");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float coord[3];
        if (simGetObjectOrientation_internal(luaToInt(L,1),luaToInt(L,2),coord)==1)
        {
            pushFloatTableOntoStack(L,3,coord);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectPosition");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        float coord[3];
        getFloatsFromTable(L,3,3,coord);
        retVal=simSetObjectPosition_internal(luaToInt(L,1),luaToInt(L,2),coord);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectOrientation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectOrientation");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        float coord[3];
        getFloatsFromTable(L,3,3,coord);
        retVal=simSetObjectOrientation_internal(luaToInt(L,1),luaToInt(L,2),coord);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointPosition");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float jointVal[1];
        if (simGetJointPosition_internal(luaToInt(L,1),jointVal)!=-1)
        {
            luaWrap_lua_pushnumber(L,jointVal[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetJointPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointPosition");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetJointPosition_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetJointTargetPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointTargetPosition");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetJointTargetPosition_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointTargetPosition(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointTargetPosition");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float targetPos;
        if (simGetJointTargetPosition_internal(luaToInt(L,1),&targetPos)!=-1)
        {
            luaWrap_lua_pushnumber(L,targetPos);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetJointMaxForce(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointMaxForce");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetJointMaxForce_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointMaxForce(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointMaxForce");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float jointF[1];
        if (simGetJointMaxForce_internal(luaToInt(L,1),jointF)>0)
        {
            luaWrap_lua_pushnumber(L,jointF[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetJointTargetVelocity(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointTargetVelocity");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetJointTargetVelocity_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointTargetVelocity(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointTargetVelocity");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float targetVel;
        if (simGetJointTargetVelocity_internal(luaToInt(L,1),&targetVel)!=-1)
        {
            luaWrap_lua_pushnumber(L,targetVel);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRefreshDialogs(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.refreshDialogs");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simRefreshDialogs_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectName");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        char* name=simGetObjectName_internal(luaToInt(L,1));
        if (name!=nullptr)
        {
            luaWrap_lua_pushstring(L,name);
            simReleaseBuffer_internal(name);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetCollectionName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getCollectionName");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        char* name=simGetCollectionName_internal(luaToInt(L,1));
        if (name!=nullptr)
        {
            luaWrap_lua_pushstring(L,name);
            simReleaseBuffer_internal(name);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetModuleName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getModuleName");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        unsigned char version;
        char* name=simGetModuleName_internal(luaToInt(L,1),&version);
        if (name!=nullptr)
        {
            luaWrap_lua_pushstring(L,name);
            simReleaseBuffer_internal(name);
            luaWrap_lua_pushinteger(L,version);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetSimulationTime(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getSimulationTime");

    float theTime=simGetSimulationTime_internal();
    if (theTime>=0.0f)
    {
        luaWrap_lua_pushnumber(L,theTime);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}

int _simGetSimulationState(luaWrap_lua_State* L)
{
    // In case we copy-paste a script during execution, the new script doesn't get the sim_simulation_starting message,
    // but that is ok!!! sim_simulation_starting is only for a simulation start. For a first run in a script, use some
    // random variable and check whether it is != from nil!! or use simGetScriptExecutionCount
    TRACE_LUA_API;
    LUA_START("sim.getSimulationState");

    int retVal=simGetSimulationState_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetSystemTime(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getSystemTime");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushnumber(L,simGetSystemTime_internal());
    LUA_END(1);
}

int _simGetSystemTimeInMs(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getSystemTimeInMs");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int lastTime=luaToInt(L,1);
        luaWrap_lua_pushinteger(L,simGetSystemTimeInMs_internal(lastTime));
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCheckCollision(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkCollision");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int entity1Handle=luaToInt(L,1);
        int entity2Handle=luaToInt(L,2);
        if (doesEntityExist(L,&errorString,entity1Handle))
        {
            if ( (entity2Handle==sim_handle_all)||doesEntityExist(L,&errorString,entity2Handle) )
            {
                if (entity2Handle==sim_handle_all)
                    entity2Handle=-1;

                if (App::currentWorld->mainSettings->collisionDetectionEnabled)
                {
                    int collidingIds[2];
                    if (CCollisionRoutine::doEntitiesCollide(entity1Handle,entity2Handle,nullptr,true,true,collidingIds))
                    {
                        luaWrap_lua_pushinteger(L,1);
                        pushIntTableOntoStack(L,2,collidingIds);
                        LUA_END(2);
                    }
                }
                luaWrap_lua_pushinteger(L,0);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCheckCollisionEx(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkCollisionEx");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float* intersections[1];
        retVal=simCheckCollisionEx_internal(luaToInt(L,1),luaToInt(L,2),intersections);
        if (retVal>0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushFloatTableOntoStack(L,retVal*6,(*intersections));
            simReleaseBuffer_internal((char*)(*intersections));
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckDistance(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkDistance");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int entity1Handle=luaToInt(L,1);
        int entity2Handle=luaToInt(L,2);
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,true,&errorString);
        if (res>=0)
        {
            float threshold=-1.0f;
            if (res==2)
                threshold=luaToFloat(L,3);
            if (doesEntityExist(L,&errorString,entity1Handle))
            {
                if ( (entity2Handle==sim_handle_all)||doesEntityExist(L,&errorString,entity2Handle) )
                {
                    if (entity2Handle==sim_handle_all)
                        entity2Handle=-1;

                    if (App::currentWorld->mainSettings->distanceCalculationEnabled)
                    {
                        int buffer[4];
                        App::currentWorld->cacheData->getCacheDataDist(entity1Handle,entity2Handle,buffer);
                        if (threshold<=0.0f)
                            threshold=SIM_MAX_FLOAT;
                        float distanceData[7];
                        bool result=CDistanceRoutine::getDistanceBetweenEntitiesIfSmaller(entity1Handle,entity2Handle,threshold,distanceData,buffer,buffer+2,true,true);
                        App::currentWorld->cacheData->setCacheDataDist(entity1Handle,entity2Handle,buffer);
                        if (result)
                        {
                            luaWrap_lua_pushinteger(L,1);
                            pushFloatTableOntoStack(L,7,distanceData);
                            int tb[2]={buffer[0],buffer[2]};
                            pushIntTableOntoStack(L,2,tb);
                            LUA_END(3);
                        }
                    }
                    luaWrap_lua_pushinteger(L,0);
                    LUA_END(1);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectConfiguration(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectConfiguration");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        char* data=simGetObjectConfiguration_internal(luaToInt(L,1));
        if (data!=nullptr)
        {
            int dataSize=((int*)data)[0]+4;
            luaWrap_lua_pushlstring(L,data,dataSize);
            delete[] data;
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectConfiguration(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectConfiguration");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        size_t l;
        const char* data=luaWrap_lua_tolstring(L,1,&l);
        retVal=simSetObjectConfiguration_internal(data);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetConfigurationTree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getConfigurationTree");

    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int id=luaToInt(L,1);
        if (id==sim_handle_self)
        {
            int objID=it->getObjectIDThatScriptIsAttachedTo_child();
            id=objID;
            if (id==-1)
                errorString=SIM_ERROR_ARGUMENT_VALID_ONLY_WITH_CHILD_SCRIPTS;
        }
        if (id!=-1)
        {
            char* data=simGetConfigurationTree_internal(id);
            if (data!=nullptr)
            {
                int dataSize=((int*)data)[0]+4;
                luaWrap_lua_pushlstring(L,data,dataSize);
                delete[] data;
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetConfigurationTree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setConfigurationTree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        size_t l;
        const char* data=luaWrap_lua_tolstring(L,1,&l);
        retVal=simSetConfigurationTree_internal(data);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetLastError(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getLastError");

    int scriptHandle=-1;
    if (luaWrap_lua_gettop(L)!=0)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
            scriptHandle=luaWrap_lua_tointeger(L,1);
    }
    else
        scriptHandle=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(scriptHandle);
    if (it!=nullptr)
    {
        luaWrap_lua_pushstring(L,it->getAndClearLastError().c_str());
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetSimulationTimeStep(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getSimulationTimeStep");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushnumber(L,simGetSimulationTimeStep_internal());
    LUA_END(1);
}

int _simGetSimulatorMessage(luaWrap_lua_State* L)
{ // Careful!! This command does not map its corresponding C-API command!! (different message pipeline)
    TRACE_LUA_API;
    LUA_START("sim.getSimulatorMessage");

    int auxVals[4];
    float aux2Vals[8];
    int aux2Cnt;
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
    int commandID=it->extractCommandFromOutsideCommandQueue(auxVals,aux2Vals,aux2Cnt);
    if (commandID!=-1)
    {
        luaWrap_lua_pushinteger(L,commandID);
        pushIntTableOntoStack(L,4,auxVals);
        if (aux2Cnt!=0)
        {
            pushFloatTableOntoStack(L,aux2Cnt,aux2Vals);
            LUA_END(3);
        }
        LUA_END(2);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}

int _simResetGraph(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetGraph");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetGraph_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleGraph(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleGraph");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simHandleGraph_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddLog(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addLog");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int v=luaWrap_lua_tointeger(L,1);
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,false,true,&errorString);
        if (res>0)
        {
            if (res==1)
                App::clearStatusbar();
            else
            {
                std::string msg(luaWrap_lua_tostring(L,2));
                CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
                std::string nm("???");
                if (it!=nullptr)
                    nm=it->getShortDescriptiveName();
                App::logScriptMsg(nm.c_str(),v,msg.c_str());
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simStopSimulation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.stopSimulation");

    int retVal=-1;// error
    retVal=simStopSimulation_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simPauseSimulation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.pauseSimulation");

    int retVal=-1;// error
    retVal=simPauseSimulation_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simStartSimulation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.startSimulation");

    int retVal=-1;// error
    retVal=simStartSimulation_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float arr[12];
        if (simGetObjectMatrix_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2),arr)==1)
        {
            pushFloatTableOntoStack(L,12,arr); // Success
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectMatrix");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,12))
    {
        float arr[12];
        getFloatsFromTable(L,3,12,arr);
        retVal=simSetObjectMatrix_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2),arr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float arr[12];
        if (simGetJointMatrix_internal(luaWrap_lua_tointeger(L,1),arr)==1)
        {
            pushFloatTableOntoStack(L,12,arr); // Success
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetSphericalJointMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setSphericalJointMatrix");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,12))
    {
        float arr[12];
        getFloatsFromTable(L,2,12,arr);
        retVal=simSetSphericalJointMatrix_internal(luaWrap_lua_tointeger(L,1),arr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simBuildIdentityMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.buildIdentityMatrix");

    float arr[12];
    simBuildIdentityMatrix_internal(arr);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    pushFloatTableOntoStack(L,12,arr);
    LUA_END(1);
}

int _simCopyMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.copyMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,12))
    {
        float arr[12];
        getFloatsFromTable(L,1,12,arr);
        pushFloatTableOntoStack(L,12,arr);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simBuildMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.buildMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,3,lua_arg_number,3))
    {
        float arr[12];
        float pos[3];
        float euler[3];
        getFloatsFromTable(L,1,3,pos);
        getFloatsFromTable(L,2,3,euler);
        if (simBuildMatrix_internal(pos,euler,arr)==1)
        {
            pushFloatTableOntoStack(L,12,arr);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetEulerAnglesFromMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getEulerAnglesFromMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,12))
    {
        float arr[12];
        float euler[3];
        getFloatsFromTable(L,1,12,arr);
        if (simGetEulerAnglesFromMatrix_internal(arr,euler)==1)
        {
            pushFloatTableOntoStack(L,3,euler);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simInvertMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.invertMatrix");
    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,12))
    {
        float arr[12];
        getFloatsFromTable(L,1,12,arr);
        retVal=simInvertMatrix_internal(arr);
        insertFloatsIntoTableAlreadyOnStack(L,1,12,arr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simMultiplyMatrices(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.multiplyMatrices");

    if (checkInputArguments(L,&errorString,lua_arg_number,12,lua_arg_number,12))
    {
        float inM0[12];
        float inM1[12];
        float outM[12];
        getFloatsFromTable(L,1,12,inM0);
        getFloatsFromTable(L,2,12,inM1);
        if (simMultiplyMatrices_internal(inM0,inM1,outM)!=-1)
        {
            pushFloatTableOntoStack(L,12,outM);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simInterpolateMatrices(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.interpolateMatrices");

    if (checkInputArguments(L,&errorString,lua_arg_number,12,lua_arg_number,12,lua_arg_number,0))
    {
        float inM0[12];
        float inM1[12];
        float outM[12];
        getFloatsFromTable(L,1,12,inM0);
        getFloatsFromTable(L,2,12,inM1);
        if (simInterpolateMatrices_internal(inM0,inM1,luaToFloat(L,3),outM)!=-1)
        {
            pushFloatTableOntoStack(L,12,outM);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simMultiplyVector(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.multiplyVector");

    if (checkInputArguments(L,&errorString,lua_arg_number,12,lua_arg_number,3))
    {
        float matr[12];
        std::vector<float> vect;
        size_t cnt=luaWrap_lua_rawlen(L,2)/3;
        vect.resize(cnt*3);
        getFloatsFromTable(L,1,12,matr);
        getFloatsFromTable(L,2,cnt*3,&vect[0]);

        C4X4Matrix m;
        m.copyFromInterface(matr);
        for (size_t i=0;i<cnt;i++)
        {
            C3Vector v(&vect[3*i]);
            (m*v).copyTo(&vect[3*i]);
        }
        pushFloatTableOntoStack(L,3*cnt,&vect[0]);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectParent(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectParent");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetObjectParent_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectChild(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectChild");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simGetObjectChild_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectParent(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectParent");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_bool,0))
        retVal=simSetObjectParent_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2),luaWrap_lua_toboolean(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectType(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectType");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetObjectType_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointType(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointType");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetJointType_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetBoolParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setBoolParameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0))
        retVal=simSetBoolParameter_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_toboolean(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetBoolParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getBoolParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int retVal=simGetBoolParameter_internal(luaWrap_lua_tointeger(L,1));
        if (retVal!=-1)
        {
            luaWrap_lua_pushboolean(L,retVal!=0);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setInt32Parameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int paramIndex=luaWrap_lua_tointeger(L,1);
        int v=luaWrap_lua_tointeger(L,2);
        if (paramIndex==sim_intparam_error_report_mode)
        { // for backward compatibility (2020)
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
            if (it!=nullptr)
            {
                bool r=true; // default
                if ( (v&sim_api_error_report)==0 )
                    r=false;
                it->setRaiseErrors_backCompatibility(r);
                retVal=1;
            }
        }
        else
            retVal=simSetInt32Parameter_internal(paramIndex,v);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getInt32Parameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int paramIndex=luaWrap_lua_tointeger(L,1);
        if (paramIndex==sim_intparam_error_report_mode)
        { // for backward compatibility (2020)
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
            if (it!=nullptr)
            {
                int v=1; // default
                if (!it->getRaiseErrors_backCompatibility())
                    v=0;
                luaWrap_lua_pushinteger(L,v);
                LUA_END(1);
            }
        }
        else
        {
            int v;
            int retVal=simGetInt32Parameter_internal(paramIndex,&v);
            if (retVal!=-1)
            {
                luaWrap_lua_pushinteger(L,v);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setFloatParameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetFloatParameter_internal(luaWrap_lua_tointeger(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getFloatParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float v;
        int retVal=simGetFloatParameter_internal(luaWrap_lua_tointeger(L,1),&v);
        if (retVal!=-1)
        {
            luaWrap_lua_pushnumber(L,v);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(1);
}

int _simSetStringParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setStringParameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
        retVal=simSetStringParameter_internal(luaToInt(L,1),luaWrap_lua_tostring(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetStringParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getStringParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        char* s=simGetStringParameter_internal(luaWrap_lua_tointeger(L,1));
        if (s!=nullptr)
        {
            luaWrap_lua_pushstring(L,s);
            delete[] s;
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetArrayParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setArrayParameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_table,0))
    {
        int parameter=luaWrap_lua_tointeger(L,1);
        if (true)
        { // for now all array parameters are tables of 3 floats
            float theArray[3];
            getFloatsFromTable(L,2,3,theArray);
            retVal=simSetArrayParameter_internal(parameter,theArray);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetArrayParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getArrayParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int parameter=luaWrap_lua_tointeger(L,1);
        if (true)
        { // for now all parameters are tables of 3 floats
            float theArray[3];
            int retVal=simGetArrayParameter_internal(parameter,theArray);
            if (retVal!=-1)
            {
                pushFloatTableOntoStack(L,3,theArray);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRemoveObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeObject");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objId=luaWrap_lua_tointeger(L,1);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if (!it->getThreadedExecution())
            retVal=simRemoveObject_internal(objId);
        else
        { // this script runs threaded and wants to destroy another object (than itself probably). We need to make sure that it will only destroy objects that do not have any scripts attached with a non-nullptr lua state:
            std::vector<CLuaScriptObject*> scripts;
            App::currentWorld->luaScriptContainer->getScriptsFromObjectAttachedTo(objId,scripts);
            bool ok=true;
            for (size_t i=0;i<scripts.size();i++)
            {
                if ( (it!=scripts[i])&&scripts[i]->hasLuaState() )
                    ok=false;
            }
            if (ok)
                retVal=simRemoveObject_internal(objId);
            else
                errorString=SIM_ERROR_THREADED_SCRIPT_DESTROYING_OBJECTS_WITH_ACTIVE_SCRIPTS;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveModel(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeModel");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objId=luaWrap_lua_tointeger(L,1);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if (!it->getThreadedExecution())
            retVal=simRemoveModel_internal(objId);
        else
        { // this script runs threaded and wants to destroy other objects. We need to make sure that it will only destroy objects that do not have any scripts attached with a non-nullptr lua state:
            CSceneObject* objBase=App::currentWorld->sceneObjects->getObjectFromHandle(objId);
            if (objBase!=nullptr)
            {
                if (objBase->getModelBase())
                {
                    std::vector<int> modelObjects;
                    modelObjects.push_back(objId);
                    CSceneObjectOperations::addRootObjectChildrenToSelection(modelObjects);
                    bool ok=true;
                    for (size_t j=0;j<modelObjects.size();j++)
                    {
                        std::vector<CLuaScriptObject*> scripts;
                        App::currentWorld->luaScriptContainer->getScriptsFromObjectAttachedTo(modelObjects[j],scripts);
                        for (size_t i=0;i<scripts.size();i++)
                        {
                            if ( (it!=scripts[i])&&scripts[i]->hasLuaState() )
                                ok=false;
                        }
                    }
                    if (ok)
                        retVal=simRemoveModel_internal(objId);
                    else
                        errorString=SIM_ERROR_THREADED_SCRIPT_DESTROYING_OBJECTS_WITH_ACTIVE_SCRIPTS;
                }
                else
                    errorString=SIM_ERROR_OBJECT_NOT_MODEL_BASE;
            }
            else
                errorString=SIM_ERROR_OBJECT_INEXISTANT;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectName");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
        retVal=simSetObjectName_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tostring(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetCollectionName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setCollectionName");
    int retVal=-1;// error

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
        retVal=simSetCollectionName_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tostring(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointInterval(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointInterval");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        simBool cyclic;
        simFloat interval[2];
        if (simGetJointInterval_internal(luaWrap_lua_tointeger(L,1),&cyclic,interval)==1)
        {
            luaWrap_lua_pushboolean(L,cyclic!=0);
            pushFloatTableOntoStack(L,2,interval);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetJointInterval(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointInterval");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0,lua_arg_number,2))
    {
        float interval[2];
        getFloatsFromTable(L,3,2,interval);
        retVal=simSetJointInterval_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_toboolean(L,2),interval);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLoadScene(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.loadScene");

    int retVal=-1;// error
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* script=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ( (script!=nullptr)&&((script->getScriptType()==sim_scripttype_addonfunction)||(script->getScriptType()==sim_scripttype_addonscript)||(script->getScriptType()==sim_scripttype_sandboxscript)) )
    {
        if (checkInputArguments(L,&errorString,lua_arg_string,0))
            retVal=simLoadScene_internal(luaWrap_lua_tostring(L,1));
    }
    else
        errorString=SIM_ERROR_MUST_BE_CALLED_FROM_ADDON_OR_SANDBOX_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCloseScene(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.closeScene");

    int retVal=-1;// error
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* script=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ( (script!=nullptr)&&((script->getScriptType()==sim_scripttype_addonfunction)||(script->getScriptType()==sim_scripttype_addonscript)||(script->getScriptType()==sim_scripttype_sandboxscript)) )
        retVal=simCloseScene_internal();
    else
        errorString=SIM_ERROR_MUST_BE_CALLED_FROM_ADDON_OR_SANDBOX_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSaveScene(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.saveScene");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
        retVal=simSaveScene_internal(luaWrap_lua_tostring(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLoadModel(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.loadModel");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int res=checkOneGeneralInputArgument(L,2,lua_arg_bool,0,true,true,&errorString);
        if (res>=0)
        {
            bool onlyThumbnails=false;
            if (res==2)
                onlyThumbnails=luaWrap_lua_toboolean(L,2);
            size_t dataLength;
            const char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength));
            if (dataLength<1000)
            { // loading from file:
                std::string path(data,dataLength);
                size_t atCopyPos=path.find("@copy");
                bool forceAsCopy=(atCopyPos!=std::string::npos);
                if (forceAsCopy)
                    path.erase(path.begin()+atCopyPos,path.end());

                if (VFile::doesFileExist(path.c_str()))
                {
                    if (CFileOperations::loadModel(path.c_str(),false,false,false,false,nullptr,onlyThumbnails,forceAsCopy))
                    {
                        if (onlyThumbnails)
                        {
                            char* buff=new char[128*128*4];
                            bool opRes=App::currentWorld->environment->modelThumbnail_notSerializedHere.copyUncompressedImageToBuffer(buff);
                            if (opRes)
                            {
                                luaWrap_lua_pushlstring(L,buff,128*128*4);
                                delete[] buff;
                                LUA_END(1);
                            }
                            delete[] buff;
                            LUA_END(0);
                        }
                        else
                            retVal=App::currentWorld->sceneObjects->getLastSelectionHandle();
                    }
                    else
                        errorString=SIM_ERROR_MODEL_COULD_NOT_BE_READ;
                }
                else
                    errorString=SIM_ERROR_FILE_NOT_FOUND;
            }
            else
            { // loading from buffer:
                std::vector<char> buffer(data,data+dataLength);
                if (CFileOperations::loadModel(nullptr,false,false,false,false,&buffer,onlyThumbnails,false))
                {
                    if (onlyThumbnails)
                    {
                        char* buff=new char[128*128*4];
                        bool opRes=App::currentWorld->environment->modelThumbnail_notSerializedHere.copyUncompressedImageToBuffer(buff);
                        if (opRes)
                        {
                            luaWrap_lua_pushlstring(L,buff,128*128*4);
                            delete[] buff;
                            LUA_END(1);
                        }
                        delete[] buff;
                        LUA_END(0);
                    }
                    else
                        retVal=App::currentWorld->sceneObjects->getLastSelectionHandle();
                }
                else
                    errorString=SIM_ERROR_MODEL_COULD_NOT_BE_READ;
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSaveModel(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.saveModel");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int model=luaWrap_lua_tointeger(L,1);
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
                retVal=simSaveModel_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tostring(L,2));
            else
            { // here we don't save to file, but to buffer:

                if (!App::currentWorld->environment->getSceneLocked())
                {
                    CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(model);
                    if (it!=nullptr)
                    {
                        if (it->getModelBase())
                        {
                            const std::vector<int>* initSelection=App::currentWorld->sceneObjects->getSelectedObjectHandlesPtr();
                            std::vector<char> buffer;
                            if (CFileOperations::saveModel(model,nullptr,false,false,false,&buffer))
                            {
                                luaWrap_lua_pushlstring(L,&buffer[0],buffer.size());
                                LUA_END(1);
                            }
                            else
                                errorString=SIM_ERROR_MODEL_COULD_NOT_BE_SAVED;
                            App::currentWorld->sceneObjects->setSelectedObjectHandles(initSelection);
                        }
                        else
                            errorString=SIM_ERROR_OBJECT_NOT_MODEL_BASE;
                    }
                    else
                        errorString=SIM_ERROR_OBJECT_INEXISTANT;
                }
                else
                    errorString=SIM_ERROR_SCENE_LOCKED;
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simIsObjectInSelection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.isObjectInSelection");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simIsObjectInSelection_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddObjectToSelection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addObjectToSelection");

    int retVal=-1;// error
    // We check if argument 1 is nil (special case):
    if (checkOneGeneralInputArgument(L,1,lua_arg_nil,0,false,true,nullptr)==1) // we do not generate an error message!
    {
        retVal=1; // nothing happens
    }
    else
    {
        // We check if we have a table at position 1:
        if (!luaWrap_lua_istable(L,1))
        { // It is not a table!
            if (checkInputArguments(L,nullptr,lua_arg_number,0,lua_arg_number,0)) // we don't generate an error
                retVal=simAddObjectToSelection_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));
            else
            { // Maybe we have a special case with one argument only?
                // nil is a valid argument!
                if (checkInputArguments(L,nullptr,lua_arg_nil,0)) // we don't generate an error
                    retVal=1;
                else
                {
                    if (checkInputArguments(L,&errorString,lua_arg_number,0))
                    {
                        if (luaWrap_lua_tointeger(L,1)==sim_handle_all)
                            retVal=simAddObjectToSelection_internal(luaWrap_lua_tointeger(L,1),-1);
                        else
                            checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0); // we just generate an error
                    }
                }
            }
        }
        else
        { // Ok we have a table. Now what size is it?
            int tableLen=int(luaWrap_lua_rawlen(L,1));
            int* buffer=new int[tableLen];
            if (getIntsFromTable(L,1,tableLen,buffer))
            {
                for (int i=0;i<tableLen;i++)
                {
                    if (App::currentWorld->sceneObjects->getObjectFromHandle(buffer[i])!=nullptr)
                        App::currentWorld->sceneObjects->addObjectToSelection(buffer[i]);
                }
                retVal=1;
            }
            else
                errorString=SIM_ERROR_TABLE_CONTAINS_INVALID_TYPES;
            delete[] buffer;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveObjectFromSelection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeObjectFromSelection");

    int retVal=-1;// error
    // We check if argument 1 is nil (special case):
    if (checkOneGeneralInputArgument(L,1,lua_arg_nil,0,false,true,nullptr)==1) // we do not generate an error message!
        retVal=1; // nothing happens
    else
    {
        // We check if we have a table at position 1:
        if (!luaWrap_lua_istable(L,1))
        { // It is not a table!
            if (checkInputArguments(L,nullptr,lua_arg_number,0,lua_arg_number,0)) // we don't generate an error
                retVal=simRemoveObjectFromSelection_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));
            else
            {
                if (checkInputArguments(L,&errorString,lua_arg_number,0))
                {
                    if (luaWrap_lua_tointeger(L,1)==sim_handle_all)
                        retVal=simRemoveObjectFromSelection_internal(luaWrap_lua_tointeger(L,1),-1);
                    else
                        checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0); // we just generate an error
                }
            }
        }
        else
        { // Ok we have a table. Now what size is it?
            int tableLen=int(luaWrap_lua_rawlen(L,1));
            int* buffer=new int[tableLen];
            if (getIntsFromTable(L,1,tableLen,buffer))
            {
                for (int i=0;i<tableLen;i++)
                    retVal=simRemoveObjectFromSelection_internal(sim_handle_single,buffer[i]);
            }
            else
                errorString=SIM_ERROR_TABLE_CONTAINS_INVALID_TYPES;
            delete[] buffer;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectSelection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectSelection");

    size_t selSize=App::currentWorld->sceneObjects->getSelectionCount();
    if (selSize!=0)
    {
        int* sel=new int[selSize];
        int res=simGetObjectSelection_internal(sel);
        if (res>0)
        {
            pushIntTableOntoStack(L,selSize,sel);
            delete[] sel;
            LUA_END(1);
        }
        delete[] sel;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetRealTimeSimulation(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getRealTimeSimulation");

    int retVal=simGetRealTimeSimulation_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLaunchExecutable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.launchExecutable");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string file(luaWrap_lua_tostring(L,1));
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            std::string args;
            if (res==2)
                args=luaWrap_lua_tostring(L,2);
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                int showStatus=1;
                if (res==2)
                    showStatus=luaToInt(L,3);
                int sh=VVARIOUS_SHOWNORMAL;
                if (showStatus==0)
                    sh=VVARIOUS_HIDE;
                if (VVarious::executeExternalApplication(file.c_str(),args.c_str(),App::folders->getExecutablePath().c_str(),sh)) // executable directory needed because otherwise the shellExecute command might switch directories!
                    retVal=1;
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetExtensionString(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getExtensionString");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int objHandle=luaWrap_lua_tointeger(L,1);
        int index=luaWrap_lua_tointeger(L,2);
        int res=checkOneGeneralInputArgument(L,3,lua_arg_string,0,true,true,&errorString);
        if (res>=0)
        {
            std::string key;
            if (res==2)
                key=luaWrap_lua_tostring(L,3);
            char* str=simGetExtensionString_internal(objHandle,index,key.c_str());
            if (str!=nullptr)
            {
                luaWrap_lua_pushstring(L,str);
                simReleaseBuffer_internal(str);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simComputeMassAndInertia(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.computeMassAndInertia");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int shapeHandle=luaWrap_lua_tointeger(L,1);
        float density=luaWrap_lua_tonumber(L,2);
        retVal=simComputeMassAndInertia_internal(shapeHandle,density);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTest(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.test");

    //int r=handleVerSpec_test(L);
    //if (r>=0)
    //    LUA_END(r);
    LUA_END(0);
}

int _simTextEditorOpen(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.textEditorOpen");
    int retVal=-1;

#ifdef SIM_WITH_GUI
    if (CPluginContainer::isCodeEditorPluginAvailable()&&(App::mainWindow!=nullptr))
    {
        if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
        {
            const char* arg1=luaWrap_lua_tostring(L,1);
            const char* arg2=luaWrap_lua_tostring(L,2);
            retVal=App::mainWindow->codeEditorContainer->open(arg1,arg2,getCurrentScriptHandle(L));
        }
    }
    else
        errorString="Code Editor plugin was not found.";
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTextEditorClose(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.textEditorClose");

#ifdef SIM_WITH_GUI
    if (CPluginContainer::isCodeEditorPluginAvailable()&&(App::mainWindow!=nullptr))
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int handle=luaWrap_lua_tointeger(L,1);
            int posAndSize[4];
            std::string txt;
            if (App::mainWindow->codeEditorContainer->close(handle,posAndSize,&txt,nullptr))
            {
                luaWrap_lua_pushstring(L,txt.c_str());
                pushIntTableOntoStack(L,2,posAndSize+0);
                pushIntTableOntoStack(L,2,posAndSize+2);
                LUA_END(3);
            }
        }
    }
    else
        errorString="Code Editor plugin was not found.";
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simTextEditorShow(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.textEditorShow");
    int retVal=-1;

#ifdef SIM_WITH_GUI
    if (CPluginContainer::isCodeEditorPluginAvailable()&&(App::mainWindow!=nullptr))
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int handle=luaWrap_lua_tointeger(L,1);
            bool showState=(luaWrap_lua_toboolean(L,2)!=0);
            retVal=App::mainWindow->codeEditorContainer->showOrHide(handle,showState);
            luaWrap_lua_pushinteger(L,retVal);
            LUA_END(1);
        }
    }
    else
        errorString="Code Editor plugin was not found.";
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTextEditorGetInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.textEditorGetInfo");

#ifdef SIM_WITH_GUI
    if (CPluginContainer::isCodeEditorPluginAvailable()&&(App::mainWindow!=nullptr))
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int handle=luaWrap_lua_tointeger(L,1);
            int state=App::mainWindow->codeEditorContainer->getShowState(handle);
            if (state>=0)
            {
                int posAndSize[4];
                std::string txt=App::mainWindow->codeEditorContainer->getText(handle,posAndSize);
                luaWrap_lua_pushstring(L,txt.c_str());
                pushIntTableOntoStack(L,2,posAndSize+0);
                pushIntTableOntoStack(L,2,posAndSize+2);
                luaWrap_lua_pushboolean(L,state!=0);
                LUA_END(4);
            }
            LUA_END(0);
        }
    }
    else
        errorString="Code Editor plugin was not found.";
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetJointDependency(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointDependency");
    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int jointHandle=luaWrap_lua_tointeger(L,1);
        int masterJointHandle=luaWrap_lua_tointeger(L,2);
        float off=luaWrap_lua_tonumber(L,3);
        float coeff=luaWrap_lua_tonumber(L,4);
        retVal=simSetJointDependency_internal(jointHandle,masterJointHandle,off,coeff);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointDependency(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointDependency");
    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int jointHandle=luaWrap_lua_tointeger(L,1);
        int masterJointHandle;
        float off;
        float coeff;
        retVal=simGetJointDependency_internal(jointHandle,&masterJointHandle,&off,&coeff);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,masterJointHandle);
            luaWrap_lua_pushnumber(L,off);
            luaWrap_lua_pushnumber(L,coeff);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetStackTraceback(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getLastStackTraceback");
    std::string retVal;
    int scriptHandle=-1;
    if (luaWrap_lua_gettop(L)!=0)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
            scriptHandle=luaWrap_lua_tointeger(L,1);
    }
    else
        scriptHandle=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(scriptHandle);
    if (it!=nullptr)
        retVal=it->getAndClearLastStackTraceback();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushstring(L,retVal.c_str());
    LUA_END(1);
}

int _simSetStringNamedParam(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setStringNamedParam");
    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
    {
        std::string paramName(luaWrap_lua_tostring(L,1));
        size_t l;
        const char* data=((char*)luaWrap_lua_tolstring(L,2,&l));
        retVal=simSetStringNamedParam_internal(paramName.c_str(),data,int(l));
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            LUA_END(1);
        }
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetStringNamedParam(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getStringNamedParam");
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string paramName(luaWrap_lua_tostring(L,1));
        int l;
        char* stringParam=simGetStringNamedParam_internal(paramName.c_str(),&l);
        if (stringParam!=nullptr)
        {
            luaWrap_lua_pushlstring(L,stringParam,l);
            delete[] stringParam;
            LUA_END(1);
        }
        LUA_END(0);
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetNavigationMode(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setNavigationMode");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simSetNavigationMode_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetNavigationMode(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getNavigationMode");

    int retVal=-1; //error
    retVal=simGetNavigationMode_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetPage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setPage");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simSetPage_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetPage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getPage");
    int retVal=-1; //error

    retVal=simGetPage_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCopyPasteObjects(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.copyPasteObjects");

    if (checkInputArguments(L,&errorString,lua_arg_number,1,lua_arg_number,0))
    {
        int objCnt=(int)luaWrap_lua_rawlen(L,1);
        if (checkInputArguments(L,&errorString,lua_arg_number,objCnt,lua_arg_number,0))
        {
            std::vector<int> objectHandles;
            objectHandles.resize(objCnt,0);
            getIntsFromTable(L,1,objCnt,&objectHandles[0]);
            int retCnt=simCopyPasteObjects_internal(&objectHandles[0],objCnt,luaToInt(L,2));
            pushIntTableOntoStack(L,retCnt,&objectHandles[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simScaleObjects(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.scaleObjects");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,1,lua_arg_number,0,lua_arg_bool,0))
    {
        int objCnt=(int)luaWrap_lua_rawlen(L,1);
        if (checkInputArguments(L,&errorString,lua_arg_number,objCnt,lua_arg_number,0,lua_arg_bool,0))
        {
            std::vector<int> objectHandles;
            objectHandles.resize(objCnt,0);
            getIntsFromTable(L,1,objCnt,&objectHandles[0]);
            retVal=simScaleObjects_internal(&objectHandles[0],objCnt,luaToFloat(L,2),luaToBool(L,3));
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectUniqueIdentifier(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectUniqueIdentifier");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        if (handle==sim_handle_all)
        {
            int cnt=int(App::currentWorld->sceneObjects->getObjectCount());
            int* buffer=new int[cnt];
            if (simGetObjectUniqueIdentifier_internal(handle,buffer)!=-1)
            {
                pushIntTableOntoStack(L,cnt,buffer);
                delete[] buffer;
                LUA_END(1);
            }
            delete[] buffer;
        }
        else
        {
            int retVal;
            if (simGetObjectUniqueIdentifier_internal(handle,&retVal)!=-1)
            {
                luaWrap_lua_pushinteger(L,retVal);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetThreadSwitchTiming(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setThreadSwitchTiming");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int timeInMs=luaWrap_lua_tointeger(L,1);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if (it!=nullptr)
            it->setDelayForAutoYielding(timeInMs);

        CThreadPool::setThreadSwitchTiming(timeInMs);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetThreadAutomaticSwitch(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setThreadAutomaticSwitch");

    if (luaWrap_lua_gettop(L)>0)
    {
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if (it!=nullptr)
        {
            int retVal;
            if (luaWrap_lua_isnumber(L,1))
            {
                int a=luaWrap_lua_tointeger(L,1);
                retVal=it->changeAutoYieldingForbidLevel(a,true);
            }
            else
            {
                bool allow=luaWrap_lua_toboolean(L,1);
                if (allow)
                    retVal=it->changeAutoYieldingForbidLevel(-1,false);
                else
                    retVal=it->changeAutoYieldingForbidLevel(1,false);
            }
            CThreadPool::setThreadAutomaticSwitchForbidLevel(it->getAutoYieldingForbidLevel());
            luaWrap_lua_pushinteger(L,retVal);
            LUA_END(1);
        }
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetThreadAutomaticSwitch(luaWrap_lua_State* L)
{ // doesn't generate an error
    TRACE_LUA_API;
    LUA_START("sim.getThreadAutomaticSwitch");
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    bool retVal=false;
    if (it!=nullptr)
        retVal=(it->getAutoYieldingForbidLevel()==0);

    luaWrap_lua_pushboolean(L,retVal); //CThreadPool::getThreadAutomaticSwitch());
    LUA_END(1);
}

int _simSwitchThread(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim._switchThread");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ((it!=nullptr)&&(it->canManualYield()))
    {
        if (CThreadPool::switchBackToPreviousThread())
            retVal=1;
        else
            retVal=0;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetThreadSwitchAllowed(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getThreadSwitchAllowed");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (it!=nullptr)
    {
        luaWrap_lua_pushboolean(L,it->canManualYield());
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetThreadSwitchAllowed(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setThreadSwitchAllowed");

    if (luaWrap_lua_gettop(L)>0)
    {
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if (it!=nullptr)
        {
            int retVal;
            if (luaWrap_lua_isnumber(L,1))
            {
                int a=luaWrap_lua_tointeger(L,1);
                retVal=it->changeOverallYieldingForbidLevel(a,true);
            }
            else
            {
                bool allow=luaWrap_lua_toboolean(L,1);
                int a=1;
                if (allow)
                    a=-1;
                retVal=it->changeOverallYieldingForbidLevel(a,false);
            }
            luaWrap_lua_pushinteger(L,retVal);
            LUA_END(1);
        }
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCreateCollection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createCollection");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string collName(luaWrap_lua_tostring(L,1));
        int options=luaToInt(L,2);
        retVal=simCreateCollection_internal(collName.c_str(),options);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddObjectToCollection(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addObjectToCollection");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int collHandle=luaToInt(L,1);
        int objHandle=luaToInt(L,2);
        int what=luaToInt(L,3);
        int options=luaToInt(L,4);
        retVal=simAddObjectToCollection_internal(collHandle,objHandle,what,options);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSaveImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.saveImage");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,2,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0))
    {
        size_t dataLength;
        char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength));
        std::string img(data,dataLength);
        int res[2];
        getIntsFromTable(L,2,2,res);
        int options=luaToInt(L,3);
        std::string filename(luaWrap_lua_tostring(L,4));
        int quality=luaToInt(L,5);
        int channels=3;
        if ((options&3)==1)
            channels=4;
        if ((options&3)==2)
            channels=1;
        if (int(dataLength)>=res[0]*res[1]*channels)
        {
            if ((res[0]>0)&&(res[1]>0))
            {
                if (filename.size()>0)
                {
                    if (filename[0]!='.')
                        retVal=simSaveImage_internal((unsigned char*)&img[0],res,options,filename.c_str(),quality,nullptr);
                    else
                    { // we save to memory:
                        std::string buffer;
                        retVal=CImageLoaderSaver::save((unsigned char*)&img[0],res,options,filename.c_str(),quality,&buffer);
                        if (retVal)
                        {
                            luaWrap_lua_pushlstring(L,&buffer[0],buffer.size());
                            LUA_END(1);
                        }
                        LUA_END(0);
                    }
                }
                else
                    errorString=SIM_ERROR_INVALID_ARGUMENT;
            }
            else
                errorString=SIM_ERROR_INVALID_RESOLUTION;
        }
        else
            errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLoadImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.loadImage");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int options=luaToInt(L,1);
        size_t dataLength;
        char* data=((char*)luaWrap_lua_tolstring(L,2,&dataLength));
        int resol[2];
        unsigned char* img=nullptr;
        if ( (dataLength>4)&&(data[0]=='@')&&(data[1]=='m')&&(data[2]=='e')&&(data[3]=='m') )
        {
            int reserved[1]={(int)dataLength};
            img=simLoadImage_internal(resol,options,data+4,reserved);
        }
        else
            img=simLoadImage_internal(resol,options,data,nullptr);
        if (img!=nullptr)
        {
            int s=resol[0]*resol[1]*3;
            if (options&1)
                s=resol[0]*resol[1]*4;
            luaWrap_lua_pushlstring(L,(const char*)img,s);
            delete[] ((char*)img);
            pushIntTableOntoStack(L,2,resol);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetScaledImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getScaledImage");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,2,lua_arg_number,2,lua_arg_number,0))
    {
        size_t dataLength;
        char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength));
        std::string imgIn(data,dataLength);
        int resIn[2];
        getIntsFromTable(L,2,2,resIn);
        int resOut[2];
        getIntsFromTable(L,3,2,resOut);
        int options=luaToInt(L,4);
        int channelsIn=3;
        if (options&1)
            channelsIn=4;
        if (int(dataLength)>=resIn[0]*resIn[1]*channelsIn)
        {
            if ((resIn[0]>0)&&(resIn[1]>0)&&(resOut[0]>0)&&(resOut[1]>0))
            {
                unsigned char* imgOut=simGetScaledImage_internal((unsigned char*)&imgIn[0],resIn,resOut,options,nullptr);
                if (imgOut!=nullptr)
                {
                    int s=resOut[0]*resOut[1]*3;
                    if (options&2)
                        s=resOut[0]*resOut[1]*4;
                    luaWrap_lua_pushlstring(L,(const char*)imgOut,s);
                    delete[] ((char*)imgOut);
                    pushIntTableOntoStack(L,2,resOut);
                    LUA_END(2);
                }
            }
            else
                errorString=SIM_ERROR_INVALID_RESOLUTION;
        }
        else
            errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simTransformImage(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.transformImage");
    int retVal=-1;

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,2,lua_arg_number,0))
    {
        size_t dataLength;
        char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength));
        int resol[2];
        getIntsFromTable(L,2,2,resol);
        int options=luaToInt(L,3);
        int channels=3;
        if (options&1)
            channels=4;
        if (int(dataLength)>=resol[0]*resol[1]*channels)
        {
            if ((resol[0]>0)&&(resol[1]>0))
                retVal=simTransformImage_internal((unsigned char*)data,resol,options,nullptr,nullptr,nullptr);
            else
                errorString=SIM_ERROR_INVALID_RESOLUTION;
        }
        else
            errorString=SIM_ERROR_ONE_STRING_SIZE_IS_WRONG;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetQHull(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getQHull");

    if (checkInputArguments(L,&errorString,lua_arg_number,9))
    {
        int vl=(int)luaWrap_lua_rawlen(L,1);
        if (checkInputArguments(L,&errorString,lua_arg_number,vl))
        {
            float* vertices=new float[vl];
            getFloatsFromTable(L,1,vl,vertices);
            float* vertOut;
            int vertOutL;
            int* indOut;
            int indOutL;
            if (simGetQHull_internal(vertices,vl,&vertOut,&vertOutL,&indOut,&indOutL,0,nullptr))
            {
                pushFloatTableOntoStack(L,vertOutL,vertOut);
                pushIntTableOntoStack(L,indOutL,indOut);
                delete[] vertOut;
                delete[] indOut;
                LUA_END(2);
            }
            delete[] vertices;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetDecimatedMesh(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getDecimatedMesh");

    if (checkInputArguments(L,&errorString,lua_arg_number,9,lua_arg_number,6,lua_arg_number,0))
    {
        int vl=(int)luaWrap_lua_rawlen(L,1);
        int il=(int)luaWrap_lua_rawlen(L,2);
        float percentage=luaToFloat(L,3);
        if (checkInputArguments(L,&errorString,lua_arg_number,vl,lua_arg_number,il,lua_arg_number,0))
        {
            float* vertices=new float[vl];
            getFloatsFromTable(L,1,vl,vertices);
            int* indices=new int[il];
            getIntsFromTable(L,2,il,indices);
            float* vertOut;
            int vertOutL;
            int* indOut;
            int indOutL;
            if (simGetDecimatedMesh_internal(vertices,vl,indices,il,&vertOut,&vertOutL,&indOut,&indOutL,percentage,0,nullptr))
            {
                pushFloatTableOntoStack(L,vertOutL,vertOut);
                pushIntTableOntoStack(L,indOutL,indOut);
                delete[] vertOut;
                delete[] indOut;
                LUA_END(2);
            }
            delete[] vertices;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackInt32Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packInt32Table");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(int)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            int v=luaWrap_lua_tointeger(L,-1);
                            data[4*i+0]=((char*)&v)[0];
                            data[4*i+1]=((char*)&v)[1];
                            data[4*i+2]=((char*)&v)[2];
                            data[4*i+3]=((char*)&v)[3];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(int));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackUInt32Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packUInt32Table");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(unsigned int)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            luaWrap_lua_Number na=luaWrap_lua_tonumber(L,-1);
                            if (na<0.0)
                                na=0.0;
                            unsigned int v=(unsigned int)na;
                            data[4*i+0]=((char*)&v)[0];
                            data[4*i+1]=((char*)&v)[1];
                            data[4*i+2]=((char*)&v)[2];
                            data[4*i+3]=((char*)&v)[3];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(unsigned int));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackFloatTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packFloatTable");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(float)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            float v=(float)luaWrap_lua_tonumber(L,-1);
                            data[4*i+0]=((char*)&v)[0];
                            data[4*i+1]=((char*)&v)[1];
                            data[4*i+2]=((char*)&v)[2];
                            data[4*i+3]=((char*)&v)[3];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(float));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackDoubleTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packDoubleTable");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(double)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            double v=luaWrap_lua_tonumber(L,-1);
                            data[sizeof(double)*i+0]=((char*)&v)[0];
                            data[sizeof(double)*i+1]=((char*)&v)[1];
                            data[sizeof(double)*i+2]=((char*)&v)[2];
                            data[sizeof(double)*i+3]=((char*)&v)[3];
                            data[sizeof(double)*i+4]=((char*)&v)[4];
                            data[sizeof(double)*i+5]=((char*)&v)[5];
                            data[sizeof(double)*i+6]=((char*)&v)[6];
                            data[sizeof(double)*i+7]=((char*)&v)[7];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(double));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackUInt8Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packUInt8Table");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(char)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            unsigned char v=(unsigned char)luaWrap_lua_tointeger(L,-1);
                            data[i]=((char*)&v)[0];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(char));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPackUInt16Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packUInt16Table");

    if (luaWrap_lua_gettop(L)>0)
    {
        if (luaWrap_lua_istable(L,1))
        {
            int startIndex=0;
            int count=0;
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    startIndex=luaToInt(L,2);

                res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        count=luaToInt(L,3);

                    int tableSize=int(luaWrap_lua_rawlen(L,1));

                    if (count==0)
                        count=tableSize-startIndex;
                    if (count>tableSize-startIndex)
                        count=tableSize-startIndex;
                    if (count>0)
                    {
                        char* data=new char[sizeof(short)*count];
                        for (int i=0;i<count;i++)
                        {
                            luaWrap_lua_rawgeti(L,1,i+1+startIndex);
                            unsigned short v=(unsigned short)luaWrap_lua_tointeger(L,-1);
                            data[2*i+0]=((char*)&v)[0];
                            data[2*i+1]=((char*)&v)[1];
                            luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                        }
                        luaWrap_lua_pushlstring(L,(const char*)data,count*sizeof(short));
                        delete[] data;
                        LUA_END(1);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
    }
    else
        errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackInt32Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackInt32Table");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int additionalCharOffset=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        additionalCharOffset=luaToInt(L,4);

                    size_t dataLength;
                    char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength))+additionalCharOffset;
                    dataLength=sizeof(int)*((dataLength-additionalCharOffset)/sizeof(int));
                    int packetCount=int(dataLength/sizeof(int));
                    if (count==0)
                        count=int(1999999999);

                    if ((startIndex>=0)&&(startIndex<packetCount))
                    {
                        if (startIndex+count>packetCount)
                            count=packetCount-startIndex;

                        int* theInts=new int[count];
                        for (int i=0;i<int(count);i++)
                        {
                            int v;
                            ((char*)&v)[0]=data[sizeof(int)*(i+startIndex)+0];
                            ((char*)&v)[1]=data[sizeof(int)*(i+startIndex)+1];
                            ((char*)&v)[2]=data[sizeof(int)*(i+startIndex)+2];
                            ((char*)&v)[3]=data[sizeof(int)*(i+startIndex)+3];
                            theInts[i]=v;
                        }
                        pushIntTableOntoStack(L,count,theInts);
                        delete[] theInts;
                        LUA_END(1);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackUInt32Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackUInt32Table");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int additionalCharOffset=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        additionalCharOffset=luaToInt(L,4);

                    size_t dataLength;
                    char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength))+additionalCharOffset;
                    dataLength=sizeof(unsigned int)*((dataLength-additionalCharOffset)/sizeof(unsigned int));
                    int packetCount=int(dataLength/sizeof(unsigned int));
                    if (count==0)
                        count=int(1999999999);

                    if ((startIndex>=0)&&(startIndex<packetCount))
                    {
                        if (startIndex+count>packetCount)
                            count=packetCount-startIndex;

                        unsigned int* theInts=new unsigned int[count];
                        for (int i=0;i<int(count);i++)
                        {
                            unsigned int v;
                            ((char*)&v)[0]=data[sizeof(unsigned int)*(i+startIndex)+0];
                            ((char*)&v)[1]=data[sizeof(unsigned int)*(i+startIndex)+1];
                            ((char*)&v)[2]=data[sizeof(unsigned int)*(i+startIndex)+2];
                            ((char*)&v)[3]=data[sizeof(unsigned int)*(i+startIndex)+3];
                            theInts[i]=v;
                        }
                        pushUIntTableOntoStack(L,count,theInts);
                        delete[] theInts;
                        LUA_END(1);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackFloatTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackFloatTable");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int additionalCharOffset=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        additionalCharOffset=luaToInt(L,4);

                    size_t dataLength;
                    char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength))+additionalCharOffset;
                    dataLength=sizeof(float)*((dataLength-additionalCharOffset)/sizeof(float));
                    int packetCount=int(dataLength/sizeof(float));
                    if (count==0)
                        count=int(1999999999);

                    if ((startIndex>=0)&&(startIndex<packetCount))
                    {
                        if (startIndex+count>packetCount)
                            count=packetCount-startIndex;

                        float* theFloats=new float[count];
                        for (int i=0;i<int(count);i++)
                        {
                            float v;
                            ((char*)&v)[0]=data[sizeof(float)*(i+startIndex)+0];
                            ((char*)&v)[1]=data[sizeof(float)*(i+startIndex)+1];
                            ((char*)&v)[2]=data[sizeof(float)*(i+startIndex)+2];
                            ((char*)&v)[3]=data[sizeof(float)*(i+startIndex)+3];
                            theFloats[i]=v;
                        }
                        pushFloatTableOntoStack(L,count,theFloats);
                        delete[] theFloats;
                        LUA_END(1);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackDoubleTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackDoubleTable");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int additionalCharOffset=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        additionalCharOffset=luaToInt(L,4);

                    size_t dataLength;
                    char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength))+additionalCharOffset;
                    dataLength=sizeof(double)*((dataLength-additionalCharOffset)/sizeof(double));
                    int packetCount=int(dataLength/sizeof(double));
                    if (count==0)
                        count=int(1999999999);

                    if ((startIndex>=0)&&(startIndex<packetCount))
                    {
                        if (startIndex+count>packetCount)
                            count=packetCount-startIndex;

                        double* theDoubles=new double[count];
                        for (int i=0;i<int(count);i++)
                        {
                            double v;
                            ((char*)&v)[0]=data[sizeof(double)*(i+startIndex)+0];
                            ((char*)&v)[1]=data[sizeof(double)*(i+startIndex)+1];
                            ((char*)&v)[2]=data[sizeof(double)*(i+startIndex)+2];
                            ((char*)&v)[3]=data[sizeof(double)*(i+startIndex)+3];
                            ((char*)&v)[4]=data[sizeof(double)*(i+startIndex)+4];
                            ((char*)&v)[5]=data[sizeof(double)*(i+startIndex)+5];
                            ((char*)&v)[6]=data[sizeof(double)*(i+startIndex)+6];
                            ((char*)&v)[7]=data[sizeof(double)*(i+startIndex)+7];
                            theDoubles[i]=v;
                        }
                        pushDoubleTableOntoStack(L,count,theDoubles);
                        delete[] theDoubles;
                        LUA_END(1);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackUInt8Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackUInt8Table");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                size_t dataLength;
                const char* data=(char*)luaWrap_lua_tolstring(L,1,&dataLength);
                int packetCount=(int)dataLength;
                if (count==0)
                    count=int(1999999999);

                if ((startIndex>=0)&&(startIndex<packetCount))
                {
                    if (startIndex+count>packetCount)
                        count=packetCount-startIndex;

                    int* theBytes=new int[count];
                    for (int i=0;i<count;i++)
                    {
                        unsigned char v;
                        ((char*)&v)[0]=data[i+startIndex];
                        theBytes[i]=v;
                    }
                    pushIntTableOntoStack(L,count,theBytes);
                    delete[] theBytes;
                    LUA_END(1);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackUInt16Table(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackUInt16Table");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int startIndex=0;
        int count=0;
        int additionalCharOffset=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                startIndex=luaToInt(L,2);

            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    count=luaToInt(L,3);

                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        additionalCharOffset=luaToInt(L,4);

                    size_t dataLength;
                    char* data=((char*)luaWrap_lua_tolstring(L,1,&dataLength))+additionalCharOffset;
                    dataLength=2*((dataLength-additionalCharOffset)/2);
                    int packetCount=(int)dataLength/2;
                    if (count==0)
                        count=int(1999999999);

                    if ((startIndex>=0)&&(startIndex<packetCount))
                    {
                        if (startIndex+count>packetCount)
                            count=packetCount-startIndex;

                        int* theWords=new int[count];
                        for (int i=0;i<int(count);i++)
                        {
                            unsigned short v;
                            ((char*)&v)[0]=data[2*(i+startIndex)+0];
                            ((char*)&v)[1]=data[2*(i+startIndex)+1];
                            theWords[i]=v;
                        }
                        pushIntTableOntoStack(L,count,theWords);
                        delete[] theWords;
                        LUA_END(1);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simTransformBuffer(luaWrap_lua_State* L)
{ // string inBuffer,number inFormat,number multiplier,number offset,number outFormat
    TRACE_LUA_API;
    LUA_START("sim.transformBuffer");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        size_t dataLength;
        int inFormat=luaToInt(L,2);
        int outFormat=luaToInt(L,5);
        bool clamp=(outFormat&sim_buffer_clamp)!=0;
        if (clamp)
            outFormat-=sim_buffer_clamp;
        bool something=false;
        if (inFormat==sim_buffer_float)
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const float* data=(const float*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % sizeof(float));
            dataLength/=sizeof(float);
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8)
                {
                   unsigned char* dat=new unsigned char[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[i]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[i]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(unsigned char)(data[i]);
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(unsigned char)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint8rgb)
                {
                   unsigned char* dat=new unsigned char[dataLength*3];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)data[i];
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(data[i]*mult+off);
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*3);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int8)
                {
                   char* dat=new char[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[i]=(v<-128.499f)?(-128):((v>127.499f)?(127):((char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[i]=(v<-128.499f)?(-128):((v>127.499f)?(127):((char)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(char)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(char)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint16)
                {
                    uint16_t* dat=new uint16_t[dataLength];
                    if (clamp)
                    {
                        if (noScalingNorOffset)
                        {
                            for (size_t i=0;i<dataLength;i++)
                            {
                                float v=data[i];
                                dat[i]=(v<0.0f)?(0):((v>65535.499f)?(65535):((uint16_t)v));
                            }
                        }
                        else
                        {
                            for (size_t i=0;i<dataLength;i++)
                            {
                                float v=data[i]*mult+off;
                                dat[i]=(v<0.0f)?(0):((v>65535.499f)?(65535):((uint16_t)v));
                            }
                        }
                    }
                    else
                    {
                        if (noScalingNorOffset)
                        {
                            for (size_t i=0;i<dataLength;i++)
                                dat[i]=(uint16_t)data[i];
                        }
                        else
                        {
                            for (size_t i=0;i<dataLength;i++)
                                dat[i]=(uint16_t)(data[i]*mult+off);
                        }
                    }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(uint16_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int16)
                {
                   int16_t* dat=new int16_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[i]=(v<-32768.499f)?(-32768):((v>32767.499f)?(32767):((int16_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[i]=(v<-32768.499f)?(-32768):((v>32767.499f)?(32767):((int16_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int16_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int16_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(int16_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint32)
                {
                   uint32_t* dat=new uint32_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[i]=(v<0.0f)?(0):((v>4294967295.499f)?(4294967295):((uint32_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[i]=(v<0.0f)?(0):((v>4294967295.499f)?(4294967295):((uint32_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint32_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint32_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(uint32_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int32)
                {
                   int32_t* dat=new int32_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i];
                               dat[i]=(v<-2147483648.499f)?(-2147483647):((v>2147483647.499f)?(2147483647):((int32_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=data[i]*mult+off;
                               dat[i]=(v<-2147483648.499f)?(-2147483647):((v>2147483647.499f)?(2147483647):((int32_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int32_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int32_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(int32_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_float)
                {
                   float* dat=new float[dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=data[i];
                   }
                   else
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=data[i]*mult+off;
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(float));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_double)
                {
                   double* dat=new double[dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=(double)data[i];
                   }
                   else
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=(double)(data[i]*mult+off);
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(double));
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if (inFormat==sim_buffer_double)
        {
            double mult=luaToDouble(L,3);
            double off=luaToDouble(L,4);
            bool noScalingNorOffset=( (mult==1.0)&&(off==0.0) );
            const double* data=(const double*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % sizeof(double));
            dataLength/=sizeof(double);
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8)
                {
                   unsigned char* dat=new unsigned char[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<0.0)?(0):((v>255.499)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<0.0)?(0):((v>255.499)?(255):((unsigned char)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(unsigned char)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(unsigned char)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint8rgb)
                {
                   unsigned char* dat=new unsigned char[dataLength*3];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[3*i+0]=(v<0.0)?(0):((v>255.499)?(255):((unsigned char)v));
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[3*i+0]=(v<0.0)?(0):((v>255.499)?(255):((unsigned char)v));
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)data[i];
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(data[i]*mult+off);
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*3);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int8)
                {
                   char* dat=new char[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<-128.499)?(-128):((v>127.499)?(127):((char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<-128.499)?(-128):((v>127.499)?(127):((char)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(char)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(char)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint16)
                {
                   uint16_t* dat=new uint16_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<0.0)?(0):((v>65535.499)?(65535):((uint16_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<0.0)?(0):((v>65535.499)?(65535):((uint16_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint16_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint16_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(uint16_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int16)
                {
                   int16_t* dat=new int16_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<-32768.499)?(-32768):((v>32767.499)?(32767):((int16_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<-32768.499)?(-32768):((v>32767.499)?(32767):((int16_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int16_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int16_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(int16_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint32)
                {
                   uint32_t* dat=new uint32_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<0.0)?(0):((v>4294967295.499)?(4294967295):((uint32_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<0.0)?(0):((v>4294967295.499)?(4294967295):((uint32_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint32_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(uint32_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(uint32_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_int32)
                {
                   int32_t* dat=new int32_t[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<-2147483648.499)?(-2147483647):((v>2147483647.499)?(2147483647):((int32_t)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<-2147483648.499)?(-2147483647):((v>2147483647.499)?(2147483647):((int32_t)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int32_t)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(int32_t)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(int32_t));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_float)
                {
                   float* dat=new float[dataLength];
                   if (clamp)
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i];
                               dat[i]=(v<-FLT_MAX)?(-FLT_MAX):((v>FLT_MAX)?(FLT_MAX):((float)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               double v=data[i]*mult+off;
                               dat[i]=(v<-FLT_MAX)?(-FLT_MAX):((v>FLT_MAX)?(FLT_MAX):((float)v));
                           }
                       }
                   }
                   else
                   {
                       if (noScalingNorOffset)
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(float)data[i];
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(float)(data[i]*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(float));
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_double)
                {
                   double* dat=new double[dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=data[i];
                   }
                   else
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=data[i]*mult+off;
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength*sizeof(double));
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if ( (inFormat==sim_buffer_uint8rgb)||(inFormat==sim_buffer_uint8bgr) )
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % 3);
            dataLength/=3;
            if (dataLength!=0)
            {
                if ( ( (inFormat==sim_buffer_uint8rgb)&&(outFormat==sim_buffer_uint8bgr) )||( (inFormat==sim_buffer_uint8bgr)&&(outFormat==sim_buffer_uint8rgb) ) )
                {
                   unsigned char* dat=new unsigned char[3*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[3*i+0]=data[3*i+2];
                           dat[3*i+1]=data[3*i+1];
                           dat[3*i+2]=data[3*i+0];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[3*i+2])*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[3*i+1])*mult+off;
                               dat[3*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[3*i+0])*mult+off;
                               dat[3*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                                dat[3*i+0]=(unsigned char)(float(data[3*i+2])*mult+off);
                                dat[3*i+1]=(unsigned char)(float(data[3*i+1])*mult+off);
                                dat[3*i+2]=(unsigned char)(float(data[3*i+0])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,3*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (inFormat==outFormat)
                {
                   unsigned char* dat=new unsigned char[3*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[3*i+0]=data[3*i+0];
                           dat[3*i+1]=data[3*i+1];
                           dat[3*i+2]=data[3*i+2];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[3*i+0])*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[3*i+1])*mult+off;
                               dat[3*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[3*i+2])*mult+off;
                               dat[3*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(float(data[3*i+0])*mult+off);
                               dat[3*i+1]=(unsigned char)(float(data[3*i+1])*mult+off);
                               dat[3*i+2]=(unsigned char)(float(data[3*i+2])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,3*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if (inFormat==sim_buffer_uint8rgba)
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % 4);
            dataLength/=4;
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8rgb)
                {
                   unsigned char* dat=new unsigned char[3*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[3*i+0]=data[4*i+0];
                           dat[3*i+1]=data[4*i+1];
                           dat[3*i+2]=data[4*i+2];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[4*i+0])*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+1])*mult+off;
                               dat[3*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+2])*mult+off;
                               dat[3*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(float(data[4*i+0])*mult+off);
                               dat[3*i+1]=(unsigned char)(float(data[4*i+1])*mult+off);
                               dat[3*i+2]=(unsigned char)(float(data[4*i+2])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,3*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==inFormat)
                {
                   unsigned char* dat=new unsigned char[4*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[4*i+0]=data[4*i+0];
                           dat[4*i+1]=data[4*i+1];
                           dat[4*i+2]=data[4*i+2];
                           dat[4*i+3]=data[4*i+3];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[4*i+0])*mult+off;
                               dat[4*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+1])*mult+off;
                               dat[4*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+2])*mult+off;
                               dat[4*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+3])*mult+off;
                               dat[4*i+3]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[4*i+0]=(unsigned char)(float(data[4*i+0])*mult+off);
                               dat[4*i+1]=(unsigned char)(float(data[4*i+1])*mult+off);
                               dat[4*i+2]=(unsigned char)(float(data[4*i+2])*mult+off);
                               dat[4*i+3]=(unsigned char)(float(data[4*i+3])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,4*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if (inFormat==sim_buffer_uint8argb)
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % 4);
            dataLength/=4;
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8rgb)
                {
                   unsigned char* dat=new unsigned char[3*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[3*i+0]=data[4*i+1];
                           dat[3*i+1]=data[4*i+2];
                           dat[3*i+2]=data[4*i+3];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[4*i+1])*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+2])*mult+off;
                               dat[3*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+3])*mult+off;
                               dat[3*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(float(data[4*i+1])*mult+off);
                               dat[3*i+1]=(unsigned char)(float(data[4*i+2])*mult+off);
                               dat[3*i+2]=(unsigned char)(float(data[4*i+3])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,3*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==inFormat)
                {
                   unsigned char* dat=new unsigned char[4*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[4*i+0]=data[4*i+0];
                           dat[4*i+1]=data[4*i+1];
                           dat[4*i+2]=data[4*i+2];
                           dat[4*i+3]=data[4*i+3];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[4*i+0])*mult+off;
                               dat[4*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+1])*mult+off;
                               dat[4*i+1]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+2])*mult+off;
                               dat[4*i+2]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               v=float(data[4*i+3])*mult+off;
                               dat[4*i+3]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[4*i+0]=(unsigned char)(float(data[4*i+0])*mult+off);
                               dat[4*i+1]=(unsigned char)(float(data[4*i+1])*mult+off);
                               dat[4*i+2]=(unsigned char)(float(data[4*i+2])*mult+off);
                               dat[4*i+3]=(unsigned char)(float(data[4*i+3])*mult+off);
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,4*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if (inFormat==sim_buffer_uint8)
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_split)
                {
                    size_t splitSize=size_t(luaToInt(L,4));
                    if (splitSize>0)
                    {
                        std::vector<std::string> stringTable;
                        size_t of=0;
                        while (of<dataLength)
                        {
                            size_t of2=of+splitSize;
                            if (of2>dataLength)
                                of2=dataLength;
                            std::string astr;
                            astr.assign(data+of,data+of2);
                            stringTable.push_back(astr);
                            of+=splitSize;
                        }
                        pushLStringTableOntoStack(L,stringTable);
                        LUA_END(1);
                    }
                }
                if (outFormat==sim_buffer_base64)
                {
                    std::string inDat(data,data+dataLength);
                    std::string outDat(CTTUtil::encode64(inDat));
                    luaWrap_lua_pushlstring(L,outDat.c_str(),outDat.length());
                    LUA_END(1);
                }
                if (outFormat==sim_buffer_uint8)
                {
                   unsigned char* dat=new unsigned char[dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=data[i];
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[i])*mult+off;
                               dat[i]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                               dat[i]=(unsigned char)(float(data[i])*mult+off);
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                if (outFormat==sim_buffer_uint8rgb)
                {
                   unsigned char* dat=new unsigned char[3*dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                       {
                           dat[3*i+0]=data[i];
                           dat[3*i+1]=data[i];
                           dat[3*i+2]=data[i];
                       }
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=float(data[i])*mult+off;
                               dat[3*i+0]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               dat[3*i+0]=(unsigned char)(float(data[i])*mult+off);
                               dat[3*i+1]=dat[3*i+0];
                               dat[3*i+2]=dat[3*i+0];
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,3*dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if ( (inFormat==sim_buffer_uint8rgb)||(inFormat==sim_buffer_uint8bgr) )
        {
            float mult=luaToFloat(L,3);
            float off=luaToFloat(L,4);
            bool noScalingNorOffset=( (mult==1.0f)&&(off==0.0f) );
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            dataLength-=(dataLength % 3);
            dataLength/=3;
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8)
                {
                   unsigned char* dat=new unsigned char[dataLength];
                   if (noScalingNorOffset)
                   {
                       for (size_t i=0;i<dataLength;i++)
                           dat[i]=(int(data[3*i+0])+int(data[3*i+1])+int(data[3*i+2]))/3;
                   }
                   else
                   {
                       if (clamp)
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=off+(float(data[3*i+0])+float(data[3*i+1])+float(data[3*i+2]))*mult/3.0f;
                               dat[i]=(v<0.0f)?(0):((v>255.499f)?(255):((unsigned char)v));
                           }
                       }
                       else
                       {
                           for (size_t i=0;i<dataLength;i++)
                           {
                               float v=off+(float(data[3*i+0])+float(data[3*i+1])+float(data[3*i+2]))*mult/3.0f;
                               dat[i]=(unsigned char)v;
                           }
                       }
                   }
                   luaWrap_lua_pushlstring(L,(const char*)dat,dataLength);
                   delete[] dat;
                   LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }
        if (inFormat==sim_buffer_base64)
        {
            const unsigned char* data=(const unsigned char*)luaWrap_lua_tolstring(L,1,&dataLength);
            something=true;
            if (dataLength!=0)
            {
                if (outFormat==sim_buffer_uint8)
                {
                    std::string inDat(data,data+dataLength);
                    std::string outDat(CTTUtil::decode64(inDat));
                    luaWrap_lua_pushlstring(L,outDat.c_str(),outDat.length());
                    LUA_END(1);
                }
                errorString=SIM_ERROR_INVALID_FORMAT;
            }
            else
                errorString=SIM_ERROR_INVALID_DATA;
        }

        if (!something)
            errorString=SIM_ERROR_INVALID_FORMAT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCombineRgbImages(luaWrap_lua_State* L)
{   TRACE_LUA_API;
    LUA_START("sim.combineRgbImages");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,2,lua_arg_string,0,lua_arg_number,2,lua_arg_number,0))
    {
        size_t img1Length,img2Length;
        char* img1=(char*)luaWrap_lua_tolstring(L,1,&img1Length);
        char* img2=(char*)luaWrap_lua_tolstring(L,3,&img2Length);
        int res1[2];
        int res2[2];
        getIntsFromTable(L,2,2,res1);
        getIntsFromTable(L,4,2,res2);
        int op=luaToInt(L,5);
        if ( (img1Length>=size_t(res1[0]*res1[1]*3))&&(img2Length>=size_t(res2[0]*res2[1]*3)) )
        {
            if (op==sim_imgcomb_vertical)
            {
               if (res1[0]==res2[0])
               {
                   unsigned char* dat=new unsigned char[(res1[0]*res1[1]+res2[0]*res2[1])*3];
                   size_t l=size_t(res1[0]*res1[1]*3);
                   for (size_t i=0;i<l;i++)
                       dat[i]=img1[i];
                   for (size_t i=0;i<size_t(res2[0]*res2[1]*3);i++)
                       dat[l+i]=img2[i];
                   luaWrap_lua_pushlstring(L,(const char*)dat,(res1[0]*res1[1]+res2[0]*res2[1])*3);
                   delete[] dat;
                   LUA_END(1);
               }
               else
                   errorString=SIM_ERROR_INVALID_RESOLUTION;
            }
            if (op==sim_imgcomb_horizontal)
            {
                if (res1[1]==res2[1])
                {
                    unsigned char* dat=new unsigned char[(res1[0]*res1[1]+res2[0]*res2[1])*3];
                    for (size_t y=0;y<size_t(res1[1]);y++)
                    {
                        int off1=(int)y*res1[0]*3;
                        int off2=(int)y*res2[0]*3;
                        int off3=off1+off2;
                        for (size_t i=0;i<size_t(res1[0]*3);i++)
                            dat[off3+i]=img1[off1+i];
                        off3+=res1[0]*3;
                        for (size_t i=0;i<size_t(res2[0]*3);i++)
                            dat[off3+i]=img2[off2+i];
                    }
                    luaWrap_lua_pushlstring(L,(const char*)dat,(res1[0]*res1[1]+res2[0]*res2[1])*3);
                    delete[] dat;
                    LUA_END(1);
                }
                else
                    errorString=SIM_ERROR_INVALID_RESOLUTION;
            }
        }
        else
            errorString=SIM_ERROR_INVALID_DATA;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simSetGraphUserData(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setGraphUserData");

    int retVal=-1; // for error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0))
    {
        int graphHandle=luaWrap_lua_tointeger(L,1);
        std::string dataName(luaWrap_lua_tostring(L,2));
        float data=luaToFloat(L,3);
        retVal=simSetGraphUserData_internal(graphHandle,dataName.c_str(),data);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddDrawingObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addDrawingObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int objType=luaToInt(L,1);
        float size=luaToFloat(L,2);
        float duplicateTolerance=luaToFloat(L,3);
        int parentID=luaToInt(L,4);
        int maxItemCount=luaToInt(L,5);
        float* ambient=nullptr;
        float* specular=nullptr;
        float* emission=nullptr;
        int res=checkOneGeneralInputArgument(L,6,lua_arg_number,3,true,true,&errorString);
        int okToGo=(res!=-1);
        if (okToGo)
        {
            float ambientC[3];
            float specularC[3];
            float emissionC[6];
            if (res>0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,6,3,ambientC);
                    ambient=ambientC;
                }
                res=checkOneGeneralInputArgument(L,7,lua_arg_number,3,true,true,&errorString);
                okToGo=(res!=-1);
                if (okToGo)
                {
                    if (res>0)
                    {
                        res=checkOneGeneralInputArgument(L,8,lua_arg_number,3,true,true,&errorString);
                        okToGo=(res!=-1);
                        if (okToGo)
                        {
                            if (res>0)
                            {
                                if (res==2)
                                {
                                    getFloatsFromTable(L,8,3,specularC);
                                    specular=specularC;
                                }
                                res=checkOneGeneralInputArgument(L,9,lua_arg_number,3,true,true,&errorString);
                                okToGo=(res!=-1);
                                if (okToGo)
                                {
                                    if (res>0)
                                    {
                                        if (res==2)
                                        {
                                            // following 3 are default aux colors:
                                            emissionC[3]=0.5f;
                                            emissionC[4]=0.0f;
                                            emissionC[5]=0.0f;
                                            if (int(luaWrap_lua_rawlen(L,9))<6)
                                                getFloatsFromTable(L,9,3,emissionC);
                                            else
                                            {
                                                objType|=sim_drawing_auxchannelcolor1;
                                                getFloatsFromTable(L,9,6,emissionC);
                                            }
                                            emission=emissionC;
                                        }

                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (okToGo)
            {
                retVal=simAddDrawingObject_internal(objType,size,duplicateTolerance,parentID,maxItemCount,ambient,nullptr,specular,emission);
                if ( (retVal!=-1) )//&&((objType&sim_drawing_persistent)==0) )
                { // following condition added on 2011/01/06 so as to not remove objects created from the c/c++ interface or from an add-on:
                    int currentScriptID=getCurrentScriptHandle(L);
                    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
                    if ( (itScrObj->getScriptType()==sim_scripttype_mainscript)||(itScrObj->getScriptType()==sim_scripttype_childscript) )
                    {
                        CDrawingObject* anObj=App::currentWorld->drawingCont->getObject(retVal);
                        if (anObj!=nullptr)
                        {
                            anObj->setCreatedFromScript(true);
                            anObj->setPersistent((objType&sim_drawing_persistent)!=0);
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveDrawingObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeDrawingObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objectHandle=luaToInt(L,1);
        if (objectHandle==sim_handle_all)
        { // following condition added here on 2011/01/06 so as not to remove objects created from a c/c++ call or from add-on:
            int currentScriptID=getCurrentScriptHandle(L);
            CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
            App::currentWorld->drawingCont->removeAllObjects((itScrObj->getScriptType()==sim_scripttype_mainscript)||(itScrObj->getScriptType()==sim_scripttype_childscript),true);
            retVal=1;
        }
        else
            retVal=simRemoveDrawingObject_internal(objectHandle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddDrawingObjectItem(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addDrawingObjectItem");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int drawingObjHandle=luaToInt(L,1);
        CDrawingObject* it=App::currentWorld->drawingCont->getObject(drawingObjHandle);
        int d=3;
        if (it!=nullptr)
            d=it->verticesPerItem*3+it->normalsPerItem*3+it->otherFloatsPerItem;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,d,true,true,&errorString);
        if (res==2)
        {
            float vertex[20]; // we should have enough here!
            getFloatsFromTable(L,2,d,vertex);
            retVal=simAddDrawingObjectItem_internal(drawingObjHandle,vertex);
        }
        else
        {
            if (res>=0)
                retVal=simAddDrawingObjectItem_internal(drawingObjHandle,nullptr);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddParticleObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addParticleObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        // The 4th argument can be nil or a table. Check for that:
        if (luaWrap_lua_gettop(L)<4)
            errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;
        else
        {
            if ( (!luaWrap_lua_isnil(L,4))&&((!luaWrap_lua_istable(L,4))||(int(luaWrap_lua_rawlen(L,4))<3)) )
                errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
            else
            {
                if ( (checkOneGeneralInputArgument(L,5,lua_arg_number,0,false,false,&errorString)==2)&&
                    (checkOneGeneralInputArgument(L,6,lua_arg_number,0,false,false,&errorString)==2) )
                {
                    int objType=luaToInt(L,1);
                    float size=luaToFloat(L,2);
                    float massOverVolume=luaToFloat(L,3);
                    int paramLen=int(luaWrap_lua_rawlen(L,4));
                    paramLen=(paramLen-1)/2;
                    paramLen=paramLen*2+1;
                    void* params=nullptr;
                    if (!luaWrap_lua_isnil(L,4))
                    {
                        int intParams[30];
                        float floatParams[30];
                        getIntsFromTable(L,4,paramLen,intParams);
                        getFloatsFromTable(L,4,paramLen,floatParams);
                        params=new char[paramLen*sizeof(int)];
                        ((int*)params)[0]=std::min<int>(intParams[0],(paramLen-1)/2);
                        for (int i=0;i<(paramLen-1)/2;i++)
                        {
                            ((int*)params)[1+2*i]=intParams[1+2*i+0];
                            ((float*)params)[1+2*i+1]=floatParams[1+2*i+1];
                        }
                    }
                    float lifeTime=luaToFloat(L,5);
                    int maxItemCount=luaToInt(L,6);
                    float* ambient=nullptr;
                    float* specular=nullptr;
                    float* emission=nullptr;
                    int res=checkOneGeneralInputArgument(L,7,lua_arg_number,3,true,true,&errorString);
                    int okToGo=(res!=-1);
                    if (okToGo)
                    {
                        float ambientC[3];
                        float specularC[3];
                        float emissionC[3];
                        if (res>0)
                        {
                            if (res==2)
                            {
                                getFloatsFromTable(L,7,3,ambientC);
                                ambient=ambientC;
                            }
                            res=checkOneGeneralInputArgument(L,8,lua_arg_number,3,true,true,&errorString);
                            okToGo=(res!=-1);
                            if (okToGo)
                            {
                                if (res>0)
                                {
                                    res=checkOneGeneralInputArgument(L,9,lua_arg_number,3,true,true,&errorString);
                                    okToGo=(res!=-1);
                                    if (okToGo)
                                    {
                                        if (res>0)
                                        {
                                            if (res==2)
                                            {
                                                getFloatsFromTable(L,9,3,specularC);
                                                specular=specularC;
                                            }
                                            res=checkOneGeneralInputArgument(L,10,lua_arg_number,3,true,true,&errorString);
                                            okToGo=(res!=-1);
                                            if (okToGo)
                                            {
                                                if (res>0)
                                                {
                                                    if (res==2)
                                                    {
                                                        getFloatsFromTable(L,10,3,emissionC);
                                                        emission=emissionC;
                                                    }

                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (okToGo)
                        {
                            retVal=simAddParticleObject_internal(objType,size,massOverVolume,params,lifeTime,maxItemCount,ambient,nullptr,specular,emission);
                        }
                    }
                    delete[] ((char*)params);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveParticleObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeParticleObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simRemoveParticleObject_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddParticleObjectItem(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addParticleObjectItem");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int particleObjHandle=luaToInt(L,1);
        int d=6+CPluginContainer::dyn_getParticleObjectOtherFloatsPerItem(particleObjHandle);
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,d,true,true,&errorString);
        if (res==2)
        {
            float vertex[20]; // we should have enough here!
            getFloatsFromTable(L,2,d,vertex);
            retVal=simAddParticleObjectItem_internal(particleObjHandle,vertex);
        }
        else
        {
            if (res>=0)
                retVal=simAddParticleObjectItem_internal(particleObjHandle,nullptr);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectSizeFactor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectSizeFactor");

    float retVal=-1.0f; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetObjectSizeFactor_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushnumber(L,retVal);
    LUA_END(1);
}

int _simSetIntegerSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setIntegerSignal");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        App::currentWorld->signalContainer->setIntegerSignal(std::string(luaWrap_lua_tostring(L,1)).c_str(),luaToInt(L,2),(it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        retVal=1;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetIntegerSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getIntegerSignal");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int intVal;
        if (simGetIntegerSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),&intVal)==1)
        {
            luaWrap_lua_pushinteger(L,intVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simClearIntegerSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.clearIntegerSignal");

    int retVal=-1; //error
    int res=checkOneGeneralInputArgument(L,1,lua_arg_string,0,true,true,&errorString);
    if (res>=0)
    {
        if (res!=2)
        {
            int currentScriptID=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
            retVal=App::currentWorld->signalContainer->clearAllIntegerSignals((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        }
        else
            retVal=simClearIntegerSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetFloatSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setFloatSignal");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        App::currentWorld->signalContainer->setFloatSignal(std::string(luaWrap_lua_tostring(L,1)).c_str(),luaToFloat(L,2),(it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        retVal=1;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetFloatSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getFloatSignal");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        float floatVal;
        if (simGetFloatSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),&floatVal)==1)
        {
            luaWrap_lua_pushnumber(L,floatVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simClearFloatSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.clearFloatSignal");

    int retVal=-1; //error
    int res=checkOneGeneralInputArgument(L,1,lua_arg_string,0,true,true,&errorString);
    if (res>=0)
    {
        if (res!=2)
        {
            int currentScriptID=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
            retVal=App::currentWorld->signalContainer->clearAllFloatSignals((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        }
        else
            retVal=simClearFloatSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetDoubleSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setDoubleSignal");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        App::currentWorld->signalContainer->setDoubleSignal(std::string(luaWrap_lua_tostring(L,1)).c_str(),luaToDouble(L,2),(it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        retVal=1;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetDoubleSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getDoubleSignal");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        double doubleVal;
        if (simGetDoubleSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),&doubleVal)==1)
        {
            luaWrap_lua_pushnumber(L,doubleVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simClearDoubleSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.clearDoubleSignal");

    int retVal=-1; //error
    int res=checkOneGeneralInputArgument(L,1,lua_arg_string,0,true,true,&errorString);
    if (res>=0)
    {
        if (res!=2)
        {
            int currentScriptID=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
            retVal=App::currentWorld->signalContainer->clearAllDoubleSignals((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        }
        else
            retVal=simClearDoubleSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetStringSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setStringSignal");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
    {
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        App::currentWorld->signalContainer->setStringSignal(std::string(luaWrap_lua_tostring(L,1)).c_str(),std::string(data,dataLength),(it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        retVal=1;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetStringSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getStringSignal");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int stringLength;
        char* str=simGetStringSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),&stringLength);
        if (str!=nullptr)
        {
            luaWrap_lua_pushlstring(L,str,stringLength);
            simReleaseBuffer_internal(str);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simClearStringSignal(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.clearStringSignal");

    int retVal=-1; //error
    int res=checkOneGeneralInputArgument(L,1,lua_arg_string,0,true,true,&errorString);
    if (res>=0)
    {
        if (res!=2)
        {
            int currentScriptID=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
            retVal=App::currentWorld->signalContainer->clearAllStringSignals((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript));
        }
        else
            retVal=simClearStringSignal_internal(std::string(luaWrap_lua_tostring(L,1)).c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetSignalName(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getSignalName");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        char* str=simGetSignalName_internal(luaToInt(L,1),luaToInt(L,2));
        if (str!=nullptr)
        {
            luaWrap_lua_pushstring(L,str);
            simReleaseBuffer_internal(str);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetObjectProperty_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetObjectProperty_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectSpecialProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectSpecialProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetObjectSpecialProperty_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectSpecialProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectSpecialProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetObjectSpecialProperty_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetModelProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getModelProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetModelProperty_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetModelProperty(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setModelProperty");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetModelProperty_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadForceSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readForceSensor");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float force[3];
        float torque[3];
        retVal=simReadForceSensor_internal(luaToInt(L,1),force,torque);
        if (!( (retVal==-1)||((retVal&1)==0) ))
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushFloatTableOntoStack(L,3,force);
            pushFloatTableOntoStack(L,3,torque);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simBreakForceSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.breakForceSensor");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simBreakForceSensor_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetLightParameters(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getLightParameters");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float ambientOld[3]={0.0f,0.0f,0.0f};
        float diffuse[3];
        float specular[3];
        retVal=simGetLightParameters_internal(luaToInt(L,1),nullptr,diffuse,specular);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushFloatTableOntoStack(L,3,ambientOld);
            pushFloatTableOntoStack(L,3,diffuse);
            pushFloatTableOntoStack(L,3,specular);
            LUA_END(4);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetLightParameters(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setLightParameters");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int objHandle=luaToInt(L,1);
        int state=luaToInt(L,2);
        float* diffuseP=nullptr;
        float* specularP=nullptr;
        float diffuse_[3]={0.0f,0.0f,0.0f};
        float specular_[3]={0.0f,0.0f,0.0f};
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,true,true,&errorString);
        if (res!=-1)
        {
            int res=checkOneGeneralInputArgument(L,4,lua_arg_number,3,true,true,&errorString);
            if (res!=-1)
            {
                if (res==2)
                { // get the data
                    getFloatsFromTable(L,4,3,diffuse_);
                    diffuseP=diffuse_;
                }
                int res=checkOneGeneralInputArgument(L,5,lua_arg_number,3,true,true,&errorString);
                if (res!=-1)
                {
                    if (res==2)
                    { // get the data
                        getFloatsFromTable(L,5,3,specular_);
                        specularP=specular_;
                    }
                    retVal=simSetLightParameters_internal(objHandle,state,nullptr,diffuseP,specularP);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetVelocity(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getVelocity");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float linVel[3];
        float angVel[3];
        int retVal=simGetVelocity_internal(luaToInt(L,1),linVel,angVel);
        if (retVal!=-1)
        {
            pushFloatTableOntoStack(L,3,linVel);
            pushFloatTableOntoStack(L,3,angVel);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetObjectVelocity(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectVelocity");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float linVel[3];
        float angVel[3];
        int retVal=simGetObjectVelocity_internal(luaToInt(L,1),linVel,angVel);
        if (retVal!=-1)
        {
            pushFloatTableOntoStack(L,3,linVel);
            pushFloatTableOntoStack(L,3,angVel);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simAddForceAndTorque(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addForceAndTorque");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float f[3]={0.0f,0.0f,0.0f};
        float t[3]={0.0f,0.0f,0.0f};
        bool err=false;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,3,true,true,&errorString);
        if (res==2)
            getFloatsFromTable(L,2,3,f);
        err=err||(res<0);
        if (!err)
        {
            int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,true,true,&errorString);
            if (res==2)
                getFloatsFromTable(L,3,3,t);
            err=err||(res<0);
            if (!err)
                retVal=simAddForceAndTorque_internal(luaToInt(L,1),f,t);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddForce(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addForce");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,3,lua_arg_number,3))
    {
        float r[3];
        float f[3];
        getFloatsFromTable(L,2,3,r);
        getFloatsFromTable(L,3,3,f);
        retVal=simAddForce_internal(luaToInt(L,1),r,f);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetExplicitHandling(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setExplicitHandling");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetExplicitHandling_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetExplicitHandling(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getExplicitHandling");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetExplicitHandling_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetLinkDummy(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getLinkDummy");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetLinkDummy_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetLinkDummy(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setLinkDummy");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetLinkDummy_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetShapeColor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setShapeColor");

    int retVal=-1; // means error
    int shapeHandle=-1;
    bool ok=false;
    bool correctColors=false;
    if (!checkInputArguments(L,nullptr,lua_arg_number,0))
    { // this section is to guarantee backward compatibility: color values have changed in the release following 3.1.3. So we need to adjust them
        if (checkInputArguments(L,&errorString,lua_arg_string,0))
        {
            std::string txt(luaWrap_lua_tostring(L,1));
            if (txt.compare(0,20,"@backCompatibility1:")==0)
            {
                txt.assign(txt.begin()+20,txt.end());
                if (tt::getValidInt(txt.c_str(),shapeHandle)) // try to extract the original number
                {
                    correctColors=true;
                    ok=true;
                }
                else
                    checkInputArguments(L,&errorString,lua_arg_number,0); // just generate an error
            }
            else
                checkInputArguments(L,&errorString,lua_arg_number,0); // just generate an error
        }
    }
    else
    {
        ok=true;
        shapeHandle=luaToInt(L,1);
    }

    if (ok)
    { // First arg ok
        std::string strTmp;
        char* str=nullptr;
        int colorComponent=0;
        bool err=false;
        bool transformColor=false;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,false,true,&errorString);
        if (res==2)
        {
            strTmp=luaWrap_lua_tostring(L,2);
            str=(char*)strTmp.c_str();
            transformColor=((strTmp.length()>1)&&(strTmp[0]=='@'));
        }
        err=err||(res<1);
        if (!err)
        {
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,false,false,&errorString);
            if (res==2)
                colorComponent=luaToInt(L,3);
            err=err||(res<2);
            if (!err)
            {
                int floatsInTableExpected=3;
                if (colorComponent==4)
                    floatsInTableExpected=1;
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,floatsInTableExpected,false,false,&errorString);
                if (res==2)
                {
                    float rgbData[3];
                    getFloatsFromTable(L,4,floatsInTableExpected,rgbData);
                    if (correctColors&&(colorComponent==0)&&(!transformColor) )
                    {
                        rgbData[0]=(rgbData[0]+0.25f)/0.85f;
                        rgbData[1]=(rgbData[1]+0.25f)/0.85f;
                        rgbData[2]=(rgbData[2]+0.25f)/0.85f;
                        float mx=std::max<float>(std::max<float>(rgbData[0],rgbData[1]),rgbData[2]);
                        if (mx>1.0f)
                        {
                            rgbData[0]/=mx;
                            rgbData[1]/=mx;
                            rgbData[2]/=mx;
                        }
                    }
                    retVal=simSetShapeColor_internal(shapeHandle,str,colorComponent,rgbData);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeColor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeColor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    { // First arg ok
        int shapeHandle=luaToInt(L,1);
        std::string strTmp;
        char* str=nullptr;
        int colorComponent=0;
        bool err=false;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,false,true,&errorString);
        if (res==2)
        {
            strTmp=luaWrap_lua_tostring(L,2);
            str=(char*)strTmp.c_str();
        }
        err=err||(res<1);
        if (!err)
        {
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,false,false,&errorString);
            if (res==2)
                colorComponent=luaToInt(L,3);
            err=err||(res<2);
            if (!err)
            {
                float rgbData[3];
                retVal=simGetShapeColor_internal(shapeHandle,str,colorComponent,rgbData);
                if (retVal>0)
                {
                    luaWrap_lua_pushinteger(L,retVal);
                    pushFloatTableOntoStack(L,3,rgbData);
                    LUA_END(2);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetDynamicObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.resetDynamicObject");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetDynamicObject_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetJointMode(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setJointMode");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetJointMode_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetJointMode(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointMode");

    int retVal=-1; // means error
    int options=0;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        retVal=simGetJointMode_internal(luaToInt(L,1),&options);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushinteger(L,options);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialOpen(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim._serialOpen");

    int retVal=-1; // means error
#ifdef SIM_WITH_SERIAL
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        size_t dataLength;
        const char* portName=luaWrap_lua_tolstring(L,1,&dataLength);
        int baudrate=luaToInt(L,2);
        retVal=App::worldContainer->serialPortContainer->serialPortOpen(true,portName,baudrate);
    }
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialClose(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim._serialClose");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        simSerialClose_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSerialSend(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.serialSend");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
        retVal=simSerialSend_internal(luaToInt(L,1),data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialRead(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim._serialRead");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        size_t maxLength=(size_t)luaToInt(L,2);
        std::string data;
        data.resize(maxLength);
        int nb=simSerialRead_internal(luaToInt(L,1),(char*)data.c_str(),(int)maxLength);
        if (nb>0)
        {
            luaWrap_lua_pushlstring(L,data.c_str(),nb);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSerialCheck(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.serialCheck");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simSerialCheck_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetContactInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getContactInfo");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int collidingObjects[2];
        float contactInfo[9];
        int index=luaToInt(L,3);
        if (index<sim_handleflag_extended)
            index+=sim_handleflag_extended;
        int res=simGetContactInfo_internal(luaToInt(L,1),luaToInt(L,2),index,collidingObjects,contactInfo);
        if (res==1)
        {
            pushIntTableOntoStack(L,2,collidingObjects);
            pushFloatTableOntoStack(L,3,contactInfo);
            pushFloatTableOntoStack(L,3,contactInfo+3);
            pushFloatTableOntoStack(L,3,contactInfo+6);
            LUA_END(4);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simAuxiliaryConsoleOpen(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.auxiliaryConsoleOpen");

    int retVal=-1; // Error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0))
    {
        int mode=luaToInt(L,3);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if ( (itScrObj->getScriptType()==sim_scripttype_mainscript)||(itScrObj->getScriptType()==sim_scripttype_childscript) )
        { // Add-ons and customization scripts do not have this restriction
            mode|=1;
        }
        if ( (itScrObj->getScriptType()!=sim_scripttype_sandboxscript)&&(itScrObj->getScriptType()!=sim_scripttype_addonscript) )
        { // Add-ons and sandbox scripts do not have this restriction
            mode|=16;
            mode-=16;
        }
        int* p=nullptr;
        int* s=nullptr;
        float* tc=nullptr;
        float* bc=nullptr;
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            int pp[2]={0,0};
            if (res==2)
            {
                getIntsFromTable(L,4,2,pp);
                p=pp;
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,2,true,true,&errorString);
            if (res>=0)
            {
                int ss[2]={0,0};
                if (res==2)
                {
                    getIntsFromTable(L,5,2,ss);
                    s=ss;
                }
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,3,true,true,&errorString);
                if (res>=0)
                {
                    float tc_[3]={0.0f,0.0f,0.0f};
                    if (res==2)
                    {
                        getFloatsFromTable(L,6,3,tc_);
                        tc=tc_;
                    }
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,3,true,true,&errorString);
                    if (res>=0)
                    {
                        float bc_[3]={0.0f,0.0f,0.0f};
                        if (res==2)
                        {
                            getFloatsFromTable(L,7,3,bc_);
                            bc=bc_;
                        }
                        retVal=simAuxiliaryConsoleOpen_internal(luaWrap_lua_tostring(L,1),luaToInt(L,2),mode,p,s,tc,bc);
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAuxiliaryConsoleClose(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.auxiliaryConsoleClose");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simAuxiliaryConsoleClose_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAuxiliaryConsoleShow(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.auxiliaryConsoleShow");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0))
        retVal=simAuxiliaryConsoleShow_internal(luaToInt(L,1),luaToBool(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAuxiliaryConsolePrint(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.auxiliaryConsolePrint");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,false,true,&errorString);
        if (res==1)
            retVal=simAuxiliaryConsolePrint_internal(luaToInt(L,1),nullptr);
        if (res==2)
            retVal=simAuxiliaryConsolePrint_internal(luaToInt(L,1),luaWrap_lua_tostring(L,2));
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simImportShape(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.importShape");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int fileType=luaToInt(L,1);
        std::string pathAndFilename(luaWrap_lua_tostring(L,2));
        int options=luaToInt(L,3);
        float identicalVerticeTolerance=luaToFloat(L,4);
        float scalingFactor=luaToFloat(L,5);
        retVal=simImportShape_internal(fileType,pathAndFilename.c_str(),options,identicalVerticeTolerance,scalingFactor);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simImportMesh(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.importMesh");

    int retValCnt=1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int fileType=luaToInt(L,1);
        std::string pathAndFilename(luaWrap_lua_tostring(L,2));
        int options=luaToInt(L,3);
        float identicalVerticeTolerance=luaToFloat(L,4);
        float scalingFactor=luaToFloat(L,5);
        float** vertices;
        int* verticesSizes;
        int** indices;
        int* indicesSizes;
        int elementCount=simImportMesh_internal(fileType,pathAndFilename.c_str(),options,identicalVerticeTolerance,scalingFactor,&vertices,&verticesSizes,&indices,&indicesSizes,nullptr,nullptr);
        if (elementCount>0)
        {
            // Vertices:
            luaWrap_lua_newtable(L);
            int tablePos=luaWrap_lua_gettop(L);
            for (int i=0;i<elementCount;i++)
            {
                luaWrap_lua_newtable(L);
                int smallTablePos=luaWrap_lua_gettop(L);
                for (int j=0;j<verticesSizes[i];j++)
                {
                    luaWrap_lua_pushnumber(L,vertices[i][j]);
                    luaWrap_lua_rawseti(L,smallTablePos,j+1);
                }
                luaWrap_lua_rawseti(L,tablePos,i+1);
            }
            // Indices:
            luaWrap_lua_newtable(L);
            tablePos=luaWrap_lua_gettop(L);
            for (int i=0;i<elementCount;i++)
            {
                luaWrap_lua_newtable(L);
                int smallTablePos=luaWrap_lua_gettop(L);
                for (int j=0;j<indicesSizes[i];j++)
                {
                    luaWrap_lua_pushinteger(L,indices[i][j]);
                    luaWrap_lua_rawseti(L,smallTablePos,j+1);
                }
                luaWrap_lua_rawseti(L,tablePos,i+1);
            }
            retValCnt=2;

            for (int i=0;i<elementCount;i++)
            {
                delete[] indices[i];
                delete[] vertices[i];
            }
            delete[] indicesSizes;
            delete[] indices;
            delete[] verticesSizes;
            delete[] vertices;
            LUA_END(retValCnt);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simExportMesh(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.exportMesh");

    int retVal=-1; // indicates an error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0))
    {
        int fileType=luaToInt(L,1);
        std::string pathAndFilename(luaWrap_lua_tostring(L,2));
        int options=luaToInt(L,3);
        float scalingFactor=luaToFloat(L,4);
        int elementCount=15487;
        if ( (luaWrap_lua_gettop(L)>=6)&&luaWrap_lua_istable(L,5)&&luaWrap_lua_istable(L,6) )
        {
            int ve=(int)luaWrap_lua_rawlen(L,5);
            int ie=(int)luaWrap_lua_rawlen(L,6);
            elementCount=std::min<int>(ve,ie);
        }
        if ( (checkOneGeneralInputArgument(L,5,lua_arg_table,elementCount,false,false,&errorString)==2)&&
                (checkOneGeneralInputArgument(L,6,lua_arg_table,elementCount,false,false,&errorString)==2) )
        {
            float** vertices;
            int* verticesSizes;
            int** indices;
            int* indicesSizes;
            vertices=new float*[elementCount];
            verticesSizes=new int[elementCount];
            indices=new int*[elementCount];
            indicesSizes=new int[elementCount];
            // Following needed if we break before (because one table in a table is not correct (or not a table)):
            for (int i=0;i<elementCount;i++)
            {
                vertices[i]=nullptr;
                verticesSizes[i]=0;
                indices[i]=nullptr;
                indicesSizes[i]=0;
            }

            bool error=false;
            for (int i=0;i<elementCount;i++)
            {
                if (!error)
                {
                    luaWrap_lua_rawgeti(L,5,i+1);
                    if (luaWrap_lua_istable(L,-1))
                    {
                        int vl=(int)luaWrap_lua_rawlen(L,-1);
                        if (checkOneGeneralInputArgument(L,luaWrap_lua_gettop(L),lua_arg_number,vl,false,false,&errorString)==2)
                        {
                            verticesSizes[i]=vl;
                            vertices[i]=new float[vl];
                            getFloatsFromTable(L,luaWrap_lua_gettop(L),vl,vertices[i]);
                        }
                        else
                            error=true;
                    }
                    else
                        error=true;
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                }
                if (!error)
                {
                    luaWrap_lua_rawgeti(L,6,i+1);
                    if (luaWrap_lua_istable(L,-1))
                    {
                        int vl=(int)luaWrap_lua_rawlen(L,-1);
                        if (checkOneGeneralInputArgument(L,luaWrap_lua_gettop(L),lua_arg_number,vl,false,false,&errorString)==2)
                        {
                            indicesSizes[i]=vl;
                            indices[i]=new int[vl];
                            getIntsFromTable(L,luaWrap_lua_gettop(L),vl,indices[i]);
                        }
                        else
                            error=true;
                    }
                    else
                        error=true;
                    luaWrap_lua_pop(L,1); // we have to pop the value that was pushed with luaWrap_lua_rawgeti
                }
            }

            if (!error)
                retVal=simExportMesh_internal(fileType,pathAndFilename.c_str(),options,scalingFactor,elementCount,(const float**)vertices,verticesSizes,(const int**)indices,indicesSizes,nullptr,nullptr);

            for (int i=0;i<elementCount;i++)
            {
                delete[] vertices[i];
                delete[] indices[i];
            }
            delete[] indicesSizes;
            delete[] indices;
            delete[] verticesSizes;
            delete[] vertices;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}



int _simCreateMeshShape(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createMeshShape");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int options=luaToInt(L,1);
        float shadingAngle=luaToFloat(L,2);

        int vl=2;
        int il=2;
        if ( (luaWrap_lua_gettop(L)>=4)&&luaWrap_lua_istable(L,3)&&luaWrap_lua_istable(L,4) )
        {
            vl=(int)luaWrap_lua_rawlen(L,3);
            il=(int)luaWrap_lua_rawlen(L,4);
        }
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,vl,false,false,&errorString);
        if (res==2)
        {
            res=checkOneGeneralInputArgument(L,4,lua_arg_number,il,false,false,&errorString);
            if (res==2)
            {
                float* vertices=new float[vl];
                int* indices=new int[il];
                getFloatsFromTable(L,3,vl,vertices);
                getIntsFromTable(L,4,il,indices);
                retVal=simCreateMeshShape_internal(options,shadingAngle,vertices,vl,indices,il,nullptr);
                delete[] indices;
                delete[] vertices;
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeMesh(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeMesh");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float* vertices;
        int verticesSize;
        int* indices;
        int indicesSize;
        float* normals;
        int result=simGetShapeMesh_internal(luaToInt(L,1),&vertices,&verticesSize,&indices,&indicesSize,&normals);
        if (result>0)
        {
            pushFloatTableOntoStack(L,verticesSize,vertices);
            pushIntTableOntoStack(L,indicesSize,indices);
            pushFloatTableOntoStack(L,indicesSize*3,normals);
            delete[] vertices;
            delete[] indices;
            delete[] normals;
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCreatePureShape(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createPureShape");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3,lua_arg_number,0))
    {
        int primitiveType=luaToInt(L,1);
        int options=luaToInt(L,2);
        float sizes[3];
        getFloatsFromTable(L,3,3,sizes);
        float mass=luaToFloat(L,4);
        int* precision=nullptr;
        int prec[2];
        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getIntsFromTable(L,5,2,prec);
                precision=prec;
            }
            retVal=simCreatePureShape_internal(primitiveType,options,sizes,mass,precision);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateHeightfieldShape(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createHeightfieldShape");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int options=luaToInt(L,1);
        float shadingAngle=luaToFloat(L,2);
        int xPointCount=luaToInt(L,3);
        int yPointCount=luaToInt(L,4);
        float xSize=luaToFloat(L,5);
        int res=checkOneGeneralInputArgument(L,6,lua_arg_number,xPointCount*yPointCount,false,false,&errorString);
        if (res==2)
        {
            float* heights=new float[xPointCount*yPointCount];
            getFloatsFromTable(L,6,xPointCount*yPointCount,heights);
            retVal=simCreateHeightfieldShape_internal(options,shadingAngle,xPointCount,yPointCount,xSize,heights);
            delete[] heights;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simAddBanner(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addBanner");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0))
    {
        std::string label(luaWrap_lua_tostring(L,1));
        float size=luaToFloat(L,2);
        int options=luaToInt(L,3);

        float* positionAndEulerAngles=nullptr;
        int parentObjectHandle=-1;
        float* labelColors=nullptr;
        float* backgroundColors=nullptr;

        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,6,true,true,&errorString);
        int okToGo=(res!=-1);
        if (okToGo)
        {
            float positionAndEulerAnglesC[6];
            if (res>0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,4,6,positionAndEulerAnglesC);
                    positionAndEulerAngles=positionAndEulerAnglesC;
                }
                res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,true,&errorString);
                okToGo=(res!=-1);
                if (okToGo)
                {
                    if (res>0)
                    {
                        if (res==2)
                            parentObjectHandle=luaToInt(L,5);
                        res=checkOneGeneralInputArgument(L,6,lua_arg_number,12,true,true,&errorString);
                        okToGo=(res!=-1);
                        if (okToGo)
                        {
                            float labelColorsC[12];
                            if (res>0)
                            {
                                if (res==2)
                                {
                                    getFloatsFromTable(L,6,12,labelColorsC);
                                    labelColors=labelColorsC;
                                }
                                res=checkOneGeneralInputArgument(L,7,lua_arg_number,12,true,true,&errorString);
                                okToGo=(res!=-1);
                                if (okToGo)
                                {
                                    float backgroundColorsC[12];
                                    if (res>0)
                                    {
                                        if (res==2)
                                        {
                                            getFloatsFromTable(L,7,12,backgroundColorsC);
                                            backgroundColors=backgroundColorsC;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (okToGo)
            {
                retVal=simAddBanner_internal(label.c_str(),size,options,positionAndEulerAngles,parentObjectHandle,labelColors,backgroundColors);
                if (retVal!=-1)
                { // following condition added on 2011/01/06 so as to not remove objects created from the c/c++ interface or an add-on:
                    int currentScriptID=getCurrentScriptHandle(L);
                    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
                    CBannerObject* anObj=App::currentWorld->bannerCont->getObject(retVal);
                    if (anObj!=nullptr)
                        anObj->setCreatedFromScript((itScrObj->getScriptType()==sim_scripttype_mainscript)||(itScrObj->getScriptType()==sim_scripttype_childscript));
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveBanner(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeBanner");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objectHandle=luaToInt(L,1);
        if (objectHandle==sim_handle_all)
        { // following condition added here on 2011/01/06 so as not to remove objects created from a C/c++ call
            App::currentWorld->bannerCont->removeAllObjects(true);
            retVal=1;
        }
        else
            retVal=simRemoveBanner_internal(objectHandle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateJoint(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createJoint");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int jointType=luaToInt(L,1);
        int jointMode=luaToInt(L,2);
        int options=luaToInt(L,3);
        float* sizes=nullptr;
        float* colorA=nullptr;
        float* colorB=nullptr;
        float s[2];
        float cA[12];
        float cB[12];
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,4,2,s);
                sizes=s;
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,12,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,5,12,cA);
                    colorA=cA;
                }
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,12,true,true,&errorString);
                if (res>=0)
                {
                    if (res==2)
                    {
                        getFloatsFromTable(L,6,12,cB);
                        colorB=cB;
                    }
                    retVal=simCreateJoint_internal(jointType,jointMode,options,sizes,colorA,colorB);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateDummy(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createDummy");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float size=luaToFloat(L,1);
        float* color=nullptr;
        float c[12];
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,12,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,2,12,c);
                color=c;
            }
            retVal=simCreateDummy_internal(size,color);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateProximitySensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createProximitySensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,8,lua_arg_number,15))
    {
        int sensorType=luaToInt(L,1);
        int subType=luaToInt(L,2);
        int options=luaToInt(L,3);
        int intParams[8];
        float floatParams[15];
        getIntsFromTable(L,4,8,intParams);
        getFloatsFromTable(L,5,15,floatParams);

        float* color=nullptr;
        float c[48];
        int res=checkOneGeneralInputArgument(L,6,lua_arg_number,48,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,6,48,c);
                color=c;
            }
            retVal=simCreateProximitySensor_internal(sensorType,subType,options,intParams,floatParams,color);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateForceSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createForceSensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,5,lua_arg_number,5))
    {
        int options=luaToInt(L,1);
        int intParams[5];
        float floatParams[5];
        getIntsFromTable(L,2,5,intParams);
        getFloatsFromTable(L,3,5,floatParams);

        float* color=nullptr;
        float c[24];
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,24,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,4,24,c);
                color=c;
            }
            retVal=simCreateForceSensor_internal(options,intParams,floatParams,color);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateVisionSensor(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createVisionSensor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,4,lua_arg_number,11))
    {
        int options=luaToInt(L,1);
        int intParams[4];
        float floatParams[11];
        getIntsFromTable(L,2,4,intParams);
        getFloatsFromTable(L,3,11,floatParams);

        float* color=nullptr;
        float c[48];
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,48,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,4,48,c);
                color=c;
            }
            retVal=simCreateVisionSensor_internal(options,intParams,floatParams,color);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simFloatingViewAdd(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.floatingViewAdd");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simFloatingViewAdd_internal(luaToFloat(L,1),luaToFloat(L,2),luaToFloat(L,3),luaToFloat(L,4),luaToInt(L,5));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simFloatingViewRemove(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.floatingViewRemove");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simFloatingViewRemove_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAdjustView(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.adjustView");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int res=checkOneGeneralInputArgument(L,4,lua_arg_string,0,true,true,&errorString);
        if (res>=0)
        {
            char* txt=nullptr;
            if (res==2)
                txt=(char*)luaWrap_lua_tostring(L,4);
            if ( (txt!=nullptr)&&(strlen(txt)>0) )
                retVal=simAdjustView_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),txt);
            else
                retVal=simAdjustView_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),nullptr);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCameraFitToView(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.cameraFitToView");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int* objPtr=nullptr;
        int options=0;
        float scaling=1.0f;
        int tableLen=2;
        if (luaWrap_lua_istable(L,2))
        {
            tableLen=int(luaWrap_lua_rawlen(L,2));
            int* buffer=new int[tableLen];
            objPtr=buffer;
            getIntsFromTable(L,2,tableLen,buffer);
        }
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,tableLen,true,true,&errorString);
        if (res>=0)
        {
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if (res>=0)
            {
                if (res==2)
                    options=luaToInt(L,3);
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
                if (res>=0)
                {
                    if (res==2)
                        scaling=luaToFloat(L,4);
                    retVal=simCameraFitToView_internal(luaToInt(L,1),tableLen,objPtr,options,scaling);
                }
            }
        }
        delete[] objPtr;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAnnounceSceneContentChange(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.announceSceneContentChange");

    int retVal=simAnnounceSceneContentChange_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectInt32Parameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int param;
        retVal=simGetObjectInt32Parameter_internal(luaToInt(L,1),luaToInt(L,2),&param);
        if (retVal>0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushinteger(L,param);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectInt32Parameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetObjectInt32Parameter_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simGetObjectFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectFloatParameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float param;
        retVal=simGetObjectFloatParameter_internal(luaToInt(L,1),luaToInt(L,2),&param);
        if (retVal>0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushnumber(L,param);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetObjectFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectFloatParameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetObjectFloatParameter_internal(luaToInt(L,1),luaToInt(L,2),luaToFloat(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectStringParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectStringParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int paramLength;
        char* strBuff=simGetObjectStringParameter_internal(luaToInt(L,1),luaToInt(L,2),&paramLength);
        if (strBuff!=nullptr)
        {
            luaWrap_lua_pushlstring(L,strBuff,paramLength);
            delete[] strBuff;
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectStringParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectStringParameter");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
    {
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,3,&dataLength);
        retVal=simSetObjectStringParameter_internal(luaToInt(L,1),luaToInt(L,2),data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetRotationAxis(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getRotationAxis");

    if (checkInputArguments(L,&errorString,lua_arg_number,12,lua_arg_number,12))
    {
        float inM0[12];
        float inM1[12];
        float outAxis[3];
        float angle;
        getFloatsFromTable(L,1,12,inM0);
        getFloatsFromTable(L,2,12,inM1);
        if (simGetRotationAxis_internal(inM0,inM1,outAxis,&angle)!=-1)
        {
            pushFloatTableOntoStack(L,3,outAxis);
            luaWrap_lua_pushnumber(L,angle);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRotateAroundAxis(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.rotateAroundAxis");

    if (checkInputArguments(L,&errorString,lua_arg_number,12,lua_arg_number,3,lua_arg_number,3,lua_arg_number,0))
    {
        float inM[12];
        float axis[3];
        float pos[3];
        float outM[12];
        getFloatsFromTable(L,1,12,inM);
        getFloatsFromTable(L,2,3,axis);
        getFloatsFromTable(L,3,3,pos);
        if (simRotateAroundAxis_internal(inM,axis,pos,luaToFloat(L,4),outM)!=-1)
        {
            pushFloatTableOntoStack(L,12,outM);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetJointForce(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getJointForce");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float jointF[1];
        if (simGetJointForce_internal(luaToInt(L,1),jointF)>0)
        {
            luaWrap_lua_pushnumber(L,jointF[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simPersistentDataWrite(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.persistentDataWrite");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
    {
        int options=0;
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                options=luaToInt(L,3);
            size_t dataLength;
            char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
            retVal=simPersistentDataWrite_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),data,(int)dataLength,options);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simPersistentDataRead(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.persistentDataRead");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        int stringLength;
        char* str=simPersistentDataRead_internal(std::string(luaWrap_lua_tostring(L,1)).c_str(),&stringLength);

        if (str!=nullptr)
        {
            luaWrap_lua_pushlstring(L,str,stringLength);
            simReleaseBuffer_internal(str);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simIsHandleValid(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.isHandleValid");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objType=-1;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                objType=luaToInt(L,2);
            retVal=simIsHandleValid_internal(luaToInt(L,1),objType);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRMLPos(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.rMLPos");

    int retVal=-43; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int dofs=luaToInt(L,1);
        double timeStep=luaWrap_lua_tonumber(L,2);
        int flags=luaToInt(L,3);
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,dofs*3,false,false,&errorString);
        if (res==2)
        {
            double* currentPosVelAccel=new double[dofs*3];
            getDoublesFromTable(L,4,dofs*3,currentPosVelAccel);
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,dofs*3,false,false,&errorString);
            if (res==2)
            {
                double* maxVelAccelJerk=new double[dofs*3];
                getDoublesFromTable(L,5,dofs*3,maxVelAccelJerk);
                res=checkOneGeneralInputArgument(L,6,lua_arg_bool,dofs,false,false,&errorString);
                if (res==2)
                {
                    char* selection=new char[dofs];
                    getBoolsFromTable(L,6,dofs,selection);
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,dofs*2,false,false,&errorString);
                    if (res==2)
                    {
                        double* targetPosVel=new double[dofs*2];
                        getDoublesFromTable(L,7,dofs*2,targetPosVel);

                        unsigned char auxData[1+4];
                        auxData[0]=1;
                        ((int*)(auxData+1))[0]=0;

                        int currentScriptID=getCurrentScriptHandle(L);
                        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
                        if ((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript))
                            ((int*)(auxData+1))[0]=1; // destroy at simulation end!
                        retVal=simRMLPos_internal(dofs,timeStep,flags,currentPosVelAccel,maxVelAccelJerk,(unsigned char*)selection,targetPosVel,auxData);
                        delete[] targetPosVel;
                    }
                    delete[] selection;
                }
                delete[] maxVelAccelJerk;
            }
            delete[] currentPosVelAccel;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRMLStep(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START_NO_CSIDE_ERROR("sim.rMLStep");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        double timeStep=luaWrap_lua_tonumber(L,2);

        // Get the Dofs of this object:
        int dofs=-1;
        {
            int auxVals[4]={0,handle,0,0};
            int replyData[4]={-1,-1,-1,-1};

            if (CPluginContainer::getPluginFromName("ReflexxesTypeIV",true)!=nullptr)
                CPluginContainer::sendEventCallbackMessageToOnePlugin("ReflexxesTypeIV",sim_message_eventcallback_rmlinfo,auxVals,nullptr,replyData);
            else
            {
                if (CPluginContainer::getPluginFromName("ReflexxesTypeII",true)!=nullptr)
                    CPluginContainer::sendEventCallbackMessageToOnePlugin("ReflexxesTypeII",sim_message_eventcallback_rmlinfo,auxVals,nullptr,replyData);
            }
            dofs=replyData[1];
        }

        if (dofs>0)
        {
            double* newPosVelAccel=new double[dofs*3];
            unsigned char auxData[1+8+8];
            auxData[0]=1;
            retVal=simRMLStep_internal(handle,timeStep,newPosVelAccel,auxData,nullptr);
            if (retVal>=0)
            {
                luaWrap_lua_pushinteger(L,retVal);
                pushDoubleTableOntoStack(L,dofs*3,newPosVelAccel);
                luaWrap_lua_pushnumber(L,((double*)(auxData+1))[0]);
                delete[] newPosVelAccel;
                LUA_END(3);
            }
            delete[] newPosVelAccel;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRMLRemove(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.rMLRemove");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        retVal=simRMLRemove_internal(handle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simRMLVel(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.rMLVel");

    int retVal=-43; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int dofs=luaToInt(L,1);
        double timeStep=luaWrap_lua_tonumber(L,2);
        int flags=luaToInt(L,3);
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,dofs*3,false,false,&errorString);
        if (res==2)
        {
            double* currentPosVelAccel=new double[dofs*3];
            getDoublesFromTable(L,4,dofs*3,currentPosVelAccel);
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,dofs*2,false,false,&errorString);
            if (res==2)
            {
                double* maxAccelJerk=new double[dofs*2];
                getDoublesFromTable(L,5,dofs*2,maxAccelJerk);
                res=checkOneGeneralInputArgument(L,6,lua_arg_bool,dofs,false,false,&errorString);
                if (res==2)
                {
                    char* selection=new char[dofs];
                    getBoolsFromTable(L,6,dofs,selection);
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,dofs,false,false,&errorString);
                    if (res==2)
                    {
                        double* targetVel=new double[dofs];
                        getDoublesFromTable(L,7,dofs,targetVel);

                        unsigned char auxData[1+4];
                        auxData[0]=1;
                        ((int*)(auxData+1))[0]=0;

                        int currentScriptID=getCurrentScriptHandle(L);
                        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
                        if ((it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript))
                            ((int*)(auxData+1))[0]=1; // destroy at simulation end!

                        retVal=simRMLVel_internal(dofs,timeStep,flags,currentPosVelAccel,maxAccelJerk,(unsigned char*)selection,targetVel,auxData);
                        delete[] targetVel;
                    }
                    delete[] selection;
                }
                delete[] maxAccelJerk;
            }
            delete[] currentPosVelAccel;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simGetObjectQuaternion(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectQuaternion");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float coord[4];
        if (simGetObjectQuaternion_internal(luaToInt(L,1),luaToInt(L,2),coord)==1)
        {
            pushFloatTableOntoStack(L,4,coord);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectQuaternion(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectQuaternion");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,4))
    {
        float coord[4];
        getFloatsFromTable(L,3,4,coord);
        retVal=simSetObjectQuaternion_internal(luaToInt(L,1),luaToInt(L,2),coord);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simBuildMatrixQ(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.buildMatrixQ");

    if (checkInputArguments(L,&errorString,lua_arg_number,3,lua_arg_number,4))
    {
        float arr[12];
        float pos[3];
        float quaternion[4];
        getFloatsFromTable(L,1,3,pos);
        getFloatsFromTable(L,2,4,quaternion);
        if (simBuildMatrixQ_internal(pos,quaternion,arr)==1)
        {
            pushFloatTableOntoStack(L,12,arr);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetQuaternionFromMatrix(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getQuaternionFromMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,12))
    {
        float arr[12];
        float quaternion[4];
        getFloatsFromTable(L,1,12,arr);
        if (simGetQuaternionFromMatrix_internal(arr,quaternion)==1)
        {
            pushFloatTableOntoStack(L,4,quaternion);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simFileDialog(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.fileDialog");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_string,0,lua_arg_string,0,lua_arg_string,0,lua_arg_string,0))
    {
        int mode=luaToInt(L,1);
        std::string title(luaWrap_lua_tostring(L,2));
        std::string startPath(luaWrap_lua_tostring(L,3));
        std::string initName(luaWrap_lua_tostring(L,4));
        std::string extName(luaWrap_lua_tostring(L,5));
        std::string ext(luaWrap_lua_tostring(L,6));
        char* pathAndName=simFileDialog_internal(mode,title.c_str(),startPath.c_str(),initName.c_str(),extName.c_str(),ext.c_str());
        if (pathAndName!=nullptr)
        {
            luaWrap_lua_pushstring(L,pathAndName);
            simReleaseBuffer_internal(pathAndName);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simMsgBox(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.msgBox");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0,lua_arg_string,0))
    {
        int dlgType=luaToInt(L,1);
        int dlgButtons=luaToInt(L,2);
        std::string title(luaWrap_lua_tostring(L,3));
        std::string message(luaWrap_lua_tostring(L,4));
        retVal=simMsgBox_internal(dlgType,dlgButtons,title.c_str(),message.c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLoadModule(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.loadModule");

    int retVal=-3; // means plugin could not be loaded
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
    {
        std::string fileAndPath(luaWrap_lua_tostring(L,1));
        std::string pluginName(luaWrap_lua_tostring(L,2));
        retVal=simLoadModule_internal(fileAndPath.c_str(),pluginName.c_str());
        if (retVal>=0)
            registerNewLuaFunctions(L); // otherwise we can only use the custom Lua functions that the plugin registers after this script has re-initialized!
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simUnloadModule(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unloadModule");

    int retVal=0; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simUnloadModule_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCallScriptFunction(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.callScriptFunction");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string funcAndScriptName(luaWrap_lua_tostring(L,1));
        int scriptHandleOrType=luaWrap_lua_tointeger(L,2);

        std::string scriptDescription;
        std::string funcName;
        size_t p=funcAndScriptName.find('@');
        if (p!=std::string::npos)
        {
            scriptDescription.assign(funcAndScriptName.begin()+p+1,funcAndScriptName.end());
            funcName.assign(funcAndScriptName.begin(),funcAndScriptName.begin()+p);
        }
        else
            funcName=funcAndScriptName;

        CLuaScriptObject* script=nullptr;
        if (scriptHandleOrType>=SIM_IDSTART_LUASCRIPT)
        { // script is identified by its ID
            script=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(scriptHandleOrType);
        }
        else
        { // the script is identified by its type sometimes also by its name
            if (scriptHandleOrType==sim_scripttype_mainscript)
                script=App::currentWorld->luaScriptContainer->getMainScript();
            if (scriptHandleOrType==sim_scripttype_childscript)
            {
                int objId=App::currentWorld->sceneObjects->getObjectHandleFromName(scriptDescription.c_str());
                script=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(objId);
            }
            if (scriptHandleOrType==sim_scripttype_customizationscript)
            {
                int objId=App::currentWorld->sceneObjects->getObjectHandleFromName(scriptDescription.c_str());
                script=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_customization(objId);
            }
            if (scriptHandleOrType==sim_scripttype_sandboxscript)
                script=App::worldContainer->sandboxScript;
            if (scriptHandleOrType==sim_scripttype_addonscript)
                script=App::worldContainer->addOnScriptContainer->getAddOnScriptFromName(scriptDescription.c_str());
        }

        if (script!=nullptr)
        {
            if (script->hasLuaState())
            {
                CInterfaceStack stack;
                stack.buildFromLuaStack(L,3);

                if (script->getThreadedExecutionIsUnderWay())
                { // very special handling here!
                    if (VThread::areThreadIDsSame(script->getThreadedScriptThreadId(),VThread::getCurrentThreadId()))
                    {
                        int rr=script->callScriptFunctionEx(funcName.c_str(),&stack);
                        if (rr>=0)
                        {
                            stack.buildOntoLuaStack(L,false);
                            LUA_END(stack.getStackSize());
                        }
                        else
                        {
                            if (rr==-3)
                                errorString=SIM_ERROR_FAILED_CALLING_SCRIPT_FUNCTION;
                            if (rr==-2)
                                errorString=SIM_ERROR_SCRIPT_FUNCTION_INEXISTANT;
                            if (rr==-1)
                                errorString=SIM_ERROR_ERROR_IN_SCRIPT_FUNCTION;
                        }
                    }
                    else
                    { // we have to execute that function via another thread!
                        void* d[4];
                        int callType=1;
                        d[0]=&callType;
                        d[1]=script;
                        d[2]=(void*)funcName.c_str();
                        d[3]=&stack;
                        int retVal=CThreadPool::callRoutineViaSpecificThread(script->getThreadedScriptThreadId(),d);
                        if (retVal>=0)
                        {
                            stack.buildOntoLuaStack(L,false);
                            LUA_END(stack.getStackSize());
                        }
                        else
                        {
                            if (retVal==-3)
                                errorString=SIM_ERROR_FAILED_CALLING_SCRIPT_FUNCTION;
                            if (retVal==-2)
                                errorString=SIM_ERROR_SCRIPT_FUNCTION_INEXISTANT;
                            if (retVal==-1)
                                errorString=SIM_ERROR_ERROR_IN_SCRIPT_FUNCTION;
                        }
                    }
                }
                else
                {
                    if (VThread::isCurrentThreadTheMainSimulationThread())
                    { // For now we don't allow non-main threads to call non-threaded scripts!
                        int rr=script->callScriptFunctionEx(funcName.c_str(),&stack);
                        if (rr>=0)
                        {
                            stack.buildOntoLuaStack(L,false);
                            LUA_END(stack.getStackSize());
                        }
                        else
                        {
                            if (rr==-3)
                                errorString=SIM_ERROR_FAILED_CALLING_SCRIPT_FUNCTION;
                            if (rr==-2)
                                errorString=SIM_ERROR_SCRIPT_FUNCTION_INEXISTANT;
                            if (rr==-1)
                                errorString=SIM_ERROR_ERROR_IN_SCRIPT_FUNCTION;
                        }
                    }
                    else
                        errorString=SIM_ERROR_FAILED_CALLING_SCRIPT_FUNCTION;
                }
            }
            else
                errorString=SIM_ERROR_SCRIPT_NOT_INITIALIZED;
        }
        else
            errorString=SIM_ERROR_SCRIPT_INEXISTANT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetShapeMass(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeMass");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float mass;
        int result=simGetShapeMass_internal(handle,&mass);
        if (result!=-1)
        {
            luaWrap_lua_pushnumber(L,mass);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetShapeMass(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setShapeMass");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float mass=luaToFloat(L,2);
        simSetShapeMass_internal(handle,mass);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetShapeInertia(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeInertia");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float inertiaMatrix[9];
        float transformation[12];
        int result=simGetShapeInertia_internal(handle,inertiaMatrix,transformation);
        if (result!=-1)
        {
            pushFloatTableOntoStack(L,9,inertiaMatrix);
            pushFloatTableOntoStack(L,12,transformation);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetShapeInertia(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setShapeInertia");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,9,lua_arg_number,12))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float inertiaMatrix[9];
        getFloatsFromTable(L,2,9,inertiaMatrix);
        float transformation[12];
        getFloatsFromTable(L,3,12,transformation);
        simSetShapeInertia_internal(handle,inertiaMatrix,transformation);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simIsDynamicallyEnabled(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.isDynamicallyEnabled");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int res=simIsDynamicallyEnabled_internal(handle);
        if (res>=0)
        {
            luaWrap_lua_pushboolean(L,res!=0);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGroupShapes(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.groupShapes");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,1))
    {
        int res=checkOneGeneralInputArgument(L,2,lua_arg_bool,0,true,true,&errorString);
        if (res>=0)
        {
            bool mergeShapes=false;
            if (res==2)
                mergeShapes=luaWrap_lua_toboolean(L,2);
            int tableSize=int(luaWrap_lua_rawlen(L,1));
            int* theTable=new int[tableSize];
            getIntsFromTable(L,1,tableSize,theTable);
            if (mergeShapes)
                tableSize=-tableSize;
            retVal=simGroupShapes_internal(theTable,tableSize);
            delete[] theTable;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simUngroupShape(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.ungroupShape");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int shapeHandle=luaWrap_lua_tointeger(L,1);
        int count;
        int* handles=simUngroupShape_internal(shapeHandle,&count);
        if (handles!=nullptr)
        {
            pushIntTableOntoStack(L,count,handles);
            delete[] handles;
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simConvexDecompose(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.convexDecompose");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int shapeHandle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int intParams[10];
        float floatParams[10];
        bool goOn=true;
        if ((options&4)==0)
        {
            goOn=false;
            int ipc=4;
            int fpc=3;
            if (options&128)
            {
                ipc=10;
                fpc=10;
            }
            int res=checkOneGeneralInputArgument(L,3,lua_arg_number,ipc,false,false,&errorString);
            if (res==2)
            {
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,fpc,false,false,&errorString);
                if (res==2)
                {
                    getIntsFromTable(L,3,ipc,intParams);
                    getFloatsFromTable(L,4,fpc,floatParams);
                    goOn=true;
                }
            }
        }
        intParams[4]=0;
        floatParams[3]=0.0f;
        floatParams[4]=0.0f;
        if (goOn)
            retVal=simConvexDecompose_internal(shapeHandle,options,intParams,floatParams);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simAddGhost(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addGhost");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int ghostGroup=luaToInt(L,1);
        int objectHandle=luaToInt(L,2);
        int options=luaToInt(L,3);
        float startTime=luaToFloat(L,4);
        float endTime=luaToFloat(L,5);
        int res=checkOneGeneralInputArgument(L,6,lua_arg_number,12,true,true,&errorString);
        if (res>=0)
        {
            bool defaultColors=true;
            float color[12];
            if (res==2)
            {
                defaultColors=false;
                getFloatsFromTable(L,6,12,color);
            }
            if (defaultColors)
                retVal=simAddGhost_internal(ghostGroup,objectHandle,options,startTime,endTime,nullptr);
            else
                retVal=simAddGhost_internal(ghostGroup,objectHandle,options,startTime,endTime,color);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simModifyGhost(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.modifyGhost");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int ghostGroup=luaToInt(L,1);
        int ghostId=luaToInt(L,2);
        int operation=luaToInt(L,3);
        float floatValue=luaToFloat(L,4);

        int options=0;
        int optionsMask=0;
        int res=0;
        if (operation==10)
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,false,false,&errorString);
        if (res>=0)
        {
            if ((res==2)&&(operation==10))
            {
                options=luaToInt(L,5);
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,false,false,&errorString);
                if (res==2)
                    optionsMask=luaToInt(L,6);
                else
                    res=-1;
            }
            if (res>=0)
            {
                int floatCnt=7;
                if (operation==13)
                    floatCnt=12;
                res=0;
                if ((operation>=11)&&(operation<=13))
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,floatCnt,false,false,&errorString);
                if (res>=0)
                {
                    float colorOrTransfData[12];
                    if ((res==2)&&(operation>=11)&&(operation<=13))
                        getFloatsFromTable(L,7,floatCnt,colorOrTransfData);
                    retVal=simModifyGhost_internal(ghostGroup,ghostId,operation,floatValue,options,optionsMask,colorOrTransfData);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simQuitSimulator(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.quitSimulator");

    simQuitSimulator_internal(false);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetShapeMaterial(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setShapeMaterial");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int shapeHandle=luaToInt(L,1);
        int materialId=luaToInt(L,2);
        retVal=simSetShapeMaterial_internal(shapeHandle,materialId);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetTextureId(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getTextureId");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string matName(luaWrap_lua_tostring(L,1));
        int resolution[2];
        retVal=simGetTextureId_internal(matName.c_str(),resolution);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushIntTableOntoStack(L,2,resolution);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReadTexture(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readTexture");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int textureId=luaToInt(L,1);
        int options=luaToInt(L,2);
        int posX=0;
        int posY=0;
        int sizeX=0;
        int sizeY=0;
        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                posX=luaToInt(L,3);
            res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    posY=luaToInt(L,4);
                res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        sizeX=luaToInt(L,5);
                    res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,false,&errorString);
                    if ((res==0)||(res==2))
                    {
                        if (res==2)
                            sizeY=luaToInt(L,6);

                        CTextureObject* to=App::currentWorld->textureContainer->getObject(textureId);
                        if (to!=nullptr)
                        {
                            int tSizeX,tSizeY;
                            to->getTextureSize(tSizeX,tSizeY);
                            if ( (posX>=0)&&(posY>=0)&&(sizeX>=0)&&(sizeY>=0)&&(posX+sizeX<=tSizeX)&&(posY+sizeY<=tSizeY) )
                            {
                                if (sizeX==0)
                                {
                                    posX=0;
                                    sizeX=tSizeX;
                                }
                                if (sizeY==0)
                                {
                                    posY=0;
                                    sizeY=tSizeY;
                                }
                                char* textureData=simReadTexture_internal(textureId,options,posX,posY,sizeX,sizeY);
                                if (textureData!=nullptr)
                                { // here we return RGB data in a string
                                    luaWrap_lua_pushlstring(L,(const char*)textureData,sizeX*sizeY*3);
                                    simReleaseBuffer_internal(textureData);
                                    LUA_END(1);
                                }
                            }
                            else
                                errorString=SIM_ERROR_INVALID_ARGUMENTS;
                        }
                        else
                            errorString=SIM_ERROR_TEXTURE_INEXISTANT;
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simWriteTexture(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.writeTexture");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
    {
        int textureId=luaToInt(L,1);
        int options=luaToInt(L,2);
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,3,&dataLength);
        int posX=0;
        int posY=0;
        int sizeX=0;
        int sizeY=0;
        float interpol=0.0f;
        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
        if ((res==0)||(res==2))
        {
            if (res==2)
                posX=luaToInt(L,4);
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
            if ((res==0)||(res==2))
            {
                if (res==2)
                    posY=luaToInt(L,5);
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,false,&errorString);
                if ((res==0)||(res==2))
                {
                    if (res==2)
                        sizeX=luaToInt(L,6);
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,0,true,false,&errorString);
                    if ((res==0)||(res==2))
                    {
                        if (res==2)
                            sizeY=luaToInt(L,7);
                        res=checkOneGeneralInputArgument(L,8,lua_arg_number,0,true,false,&errorString);
                        if ((res==0)||(res==2))
                        {
                            if (res==2)
                                interpol=luaToFloat(L,8);
                            CTextureObject* to=App::currentWorld->textureContainer->getObject(textureId);
                            if (to!=nullptr)
                            {
                                int tSizeX,tSizeY;
                                to->getTextureSize(tSizeX,tSizeY);
                                if ( (posX>=0)&&(posY>=0)&&(sizeX>=0)&&(sizeY>=0)&&(posX+sizeX<=tSizeX)&&(posY+sizeY<=tSizeY) )
                                {
                                    if (sizeX==0)
                                    {
                                        posX=0;
                                        sizeX=tSizeX;
                                    }
                                    if (sizeY==0)
                                    {
                                        posY=0;
                                        sizeY=tSizeY;
                                    }
                                    if (int(dataLength)>=sizeX*sizeY*3)
                                        retVal=simWriteTexture_internal(textureId,options,data,posX,posY,sizeX,sizeY,interpol);
                                    else
                                        errorString=SIM_ERROR_INVALID_BUFFER_SIZE;
                                }
                                else
                                    errorString=SIM_ERROR_INVALID_ARGUMENTS;
                            }
                            else
                                errorString=SIM_ERROR_TEXTURE_INEXISTANT;
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateTexture(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createTexture");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string fileName(luaWrap_lua_tostring(L,1));
        int options=luaToInt(L,2);
        float* planeSizesP=nullptr;
        float planeSizes[2];
        float* scalingUVP=nullptr;
        float scalingUV[2];
        float* xy_gP=nullptr;
        float xy_g[3];
        int resolution[2]={0,0}; // means: just any!

        // Now check the optional arguments:
        int res;
        res=checkOneGeneralInputArgument(L,3,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,3,2,planeSizes);
                planeSizesP=planeSizes;
            }

            res=checkOneGeneralInputArgument(L,4,lua_arg_number,2,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,4,2,scalingUV);
                    scalingUVP=scalingUV;
                }

                res=checkOneGeneralInputArgument(L,5,lua_arg_number,3,true,true,&errorString);
                if (res>=0)
                {
                    if (res==2)
                    {
                        getFloatsFromTable(L,5,3,xy_g);
                        xy_gP=xy_g;
                    }

                    res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,true,&errorString);
                    if (res>=0)
                    {
                        int maxTextureSize=0; // just the original
                        if (res==2)
                            maxTextureSize=luaToInt(L,6);

                        res=checkOneGeneralInputArgument(L,7,lua_arg_number,2,fileName.length()!=0,fileName.length()!=0,&errorString);
                        if (res>=0)
                        {
                            if (res==2)
                                getIntsFromTable(L,7,2,resolution);

                            if (fileName.length()==0)
                            { // we are not loading a texture, but creating it!
                                resolution[0]=tt::getLimitedInt(1,4096,resolution[0]);
                                resolution[1]=tt::getLimitedInt(1,4096,resolution[1]);
                            }

                            int textureId;
                            int shapeHandle=simCreateTexture_internal(fileName.c_str(),options,planeSizesP,scalingUVP,xy_gP,maxTextureSize,&textureId,resolution,nullptr);
                            if (shapeHandle>=0)
                            {
                                luaWrap_lua_pushinteger(L,shapeHandle);
                                luaWrap_lua_pushinteger(L,textureId);
                                pushIntTableOntoStack(L,2,resolution);
                                LUA_END(3);
                            }
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal); // error
    LUA_END(1);
}


int _simWriteCustomDataBlock(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.writeCustomDataBlock");

    int retVal=-1; // error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int objectHandle=luaToInt(L,1);
        if (objectHandle==sim_handle_self)
            objectHandle=getCurrentScriptHandle(L);

        std::string dataName(luaWrap_lua_tostring(L,2));
        int res;
        res=checkOneGeneralInputArgument(L,3,lua_arg_string,0,false,true,&errorString);
        if (res>=1)
        {
            size_t dataLength=0;
            char* data=nullptr;
            if (res==2)
                data=(char*)luaWrap_lua_tolstring(L,3,&dataLength);
            retVal=simWriteCustomDataBlock_internal(objectHandle,dataName.c_str(),data,(int)dataLength);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simReadCustomDataBlock(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readCustomDataBlock");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int objectHandle=luaToInt(L,1);
        if (objectHandle==sim_handle_self)
            objectHandle=getCurrentScriptHandle(L);

        std::string dataName(luaWrap_lua_tostring(L,2));
        int dataLength;
        char* data=simReadCustomDataBlock_internal(objectHandle,dataName.c_str(),&dataLength);
        if (data!=nullptr)
        {
            luaWrap_lua_pushlstring(L,(const char*)data,dataLength);
            simReleaseBuffer_internal(data);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simReadCustomDataBlockTags(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.readCustomDataBlockTags");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objectHandle=luaToInt(L,1);
        if (objectHandle==sim_handle_self)
            objectHandle=getCurrentScriptHandle(L);

        int tagCount;
        char* data=simReadCustomDataBlockTags_internal(objectHandle,&tagCount);
        if (data!=nullptr)
        {
            std::vector<std::string> stringTable;
            size_t off=0;
            for (int i=0;i<tagCount;i++)
            {
                stringTable.push_back(data+off);
                off+=strlen(data+off)+1;
            }
            pushStringTableOntoStack(L,stringTable);
            simReleaseBuffer_internal(data);

            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simAddPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.addPointCloud");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        int pageMask=luaToInt(L,1);
        int layerMask=luaToInt(L,2);
        int objectHandle=luaToInt(L,3);
        int options=luaToInt(L,4);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        if ( (it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript) )
            options=(options|1)-1; // cloud is automatically removed at the end of the simulation (i.e. is not persistent)
        float pointSize=luaToFloat(L,5);
        int pointCnt=(int)luaWrap_lua_rawlen(L,6)/3;
        std::vector<float> pointCoordinates(pointCnt*3,0.0f);
        getFloatsFromTable(L,6,pointCnt*3,&pointCoordinates[0]);
        int res;
        res=checkOneGeneralInputArgument(L,7,lua_arg_number,12,true,true,&errorString);
        if (res>=0)
        {
            unsigned char* defaultColors=nullptr;
            std::vector<unsigned char> _defCols(12,0);
            if (res==2)
            {
                std::vector<int> _defCols_(12,0);
                getIntsFromTable(L,7,12,&_defCols_[0]);
                for (int i=0;i<12;i++)
                    _defCols[i]=(unsigned char)_defCols_[i];
                defaultColors=&_defCols[0];
            }
            res=checkOneGeneralInputArgument(L,8,lua_arg_number,pointCnt*3,true,true,&errorString);
            if (res>=0)
            {
                unsigned char* pointColors=nullptr;
                std::vector<unsigned char> _pointCols;
                if (res==2)
                {
                    _pointCols.resize(pointCnt*3,0);
                    std::vector<int> _pointCols_(pointCnt*3,0);
                    getIntsFromTable(L,8,pointCnt*3,&_pointCols_[0]);
                    for (int i=0;i<pointCnt*3;i++)
                        _pointCols[i]=(unsigned char)_pointCols_[i];
                    pointColors=&_pointCols[0];
                }
                res=checkOneGeneralInputArgument(L,9,lua_arg_number,pointCnt*3,true,true,&errorString);
                if (res>=0)
                {
                    float* pointNormals=nullptr;
                    std::vector<float> _pointNormals;
                    if (res==2)
                    {
                        _pointNormals.resize(pointCnt*3,0);
                        getFloatsFromTable(L,9,pointCnt*3,&_pointNormals[0]);
                        pointNormals=&_pointNormals[0];
                    }
                    retVal=simAddPointCloud_internal(pageMask,layerMask,objectHandle,options,pointSize,(int)pointCoordinates.size()/3,&pointCoordinates[0],(char*)defaultColors,(char*)pointColors,pointNormals);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simModifyPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.modifyPointCloud");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int pointCloudHandle=luaToInt(L,1);
        int operation=luaToInt(L,2);
        retVal=simModifyPointCloud_internal(pointCloudHandle,operation,nullptr,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeGeomInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeGeomInfo");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        int intData[5];
        float floatData[5];
        retVal=simGetShapeGeomInfo_internal(handle,intData,floatData,nullptr);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushinteger(L,intData[0]);
            pushFloatTableOntoStack(L,4,floatData);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectsInTree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectsInTree");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        int objType=sim_handle_all;
        int options=0;
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
                objType=luaToInt(L,2);
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                    options=luaToInt(L,3);
                int objCnt=0;
                int* objHandles=simGetObjectsInTree_internal(handle,objType,options,&objCnt);
                if (objHandles!=nullptr)
                {
                    pushIntTableOntoStack(L,objCnt,objHandles);
                    simReleaseBuffer_internal((char*)objHandles);
                    LUA_END(1);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetObjectSizeValues(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setObjectSizeValues");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,3))
    {
        int handle=luaToInt(L,1);
        float s[3];
        getFloatsFromTable(L,2,3,s);
        retVal=simSetObjectSizeValues_internal(handle,s);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectSizeValues(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getObjectSizeValues");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        float s[3];
        if (simGetObjectSizeValues_internal(handle,s)!=-1)
        {
            pushFloatTableOntoStack(L,3,s);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simScaleObject(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.scaleObject");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        float x=luaToFloat(L,2);
        float y=luaToFloat(L,3);
        float z=luaToFloat(L,4);
        int options=0;
        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
                options=luaToInt(L,5);
            retVal=simScaleObject_internal(handle,x,y,z,options);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetShapeTexture(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setShapeTexture");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,2))
    {
        int handle=luaToInt(L,1);
        int textureId=luaToInt(L,2);
        int mapMode=luaToInt(L,3);
        int options=luaToInt(L,4);
        float uvScaling[2];
        getFloatsFromTable(L,5,2,uvScaling);
        float* posP=nullptr;
        float* orP=nullptr;
        float _pos[3];
        float _or[3];
        int res=checkOneGeneralInputArgument(L,6,lua_arg_number,3,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,6,3,_pos);
                posP=_pos;
            }
            res=checkOneGeneralInputArgument(L,7,lua_arg_number,3,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,7,3,_or);
                    orP=_or;
                }
                retVal=simSetShapeTexture_internal(handle,textureId,mapMode,options,uvScaling,posP,orP);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeTextureId(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeTextureId");
    int retVal=-1;

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        retVal=simGetShapeTextureId_internal(handle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetCollectionObjects(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getCollectionObjects");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        int cnt;
        int* objHandles=simGetCollectionObjects_internal(handle,&cnt);
        pushIntTableOntoStack(L,cnt,objHandles);
        delete[] objHandles;
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleCustomizationScripts(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleCustomizationScripts");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (itScrObj->getScriptType()==sim_scripttype_mainscript)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int callType=luaToInt(L,1);
            retVal=0;
            if (App::getEditModeType()==NO_EDIT_MODE)
            {
                retVal=App::currentWorld->luaScriptContainer->handleCascadedScriptExecution(sim_scripttype_customizationscript,callType,nullptr,nullptr,nullptr);
                App::currentWorld->luaScriptContainer->removeDestroyedScripts(sim_scripttype_customizationscript);
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleAddOnScripts(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleAddOnScripts");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (itScrObj->getScriptType()==sim_scripttype_mainscript)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int callType=luaToInt(L,1);
            retVal=0;
            if (App::getEditModeType()==NO_EDIT_MODE)
                retVal=App::worldContainer->addOnScriptContainer->handleAddOnScriptExecution(callType,nullptr,nullptr);
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleSandboxScript(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleSandboxScript");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (itScrObj->getScriptType()==sim_scripttype_mainscript)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int callType=luaToInt(L,1);
            if ( (App::getEditModeType()==NO_EDIT_MODE)&&(App::worldContainer->sandboxScript!=nullptr) )
                App::worldContainer->sandboxScript->runSandboxScript(callType,nullptr,nullptr);
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetScriptAttribute(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setScriptAttribute");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int scriptID=luaToInt(L,1);
        if (scriptID==sim_handle_self)
            scriptID=getCurrentScriptHandle(L);
        int attribID=luaToInt(L,2);
        int thirdArgType=lua_arg_number;
        if ( (attribID==sim_customizationscriptattribute_activeduringsimulation)||(attribID==sim_childscriptattribute_automaticcascadingcalls)||(attribID==sim_scriptattribute_enabled)||(attribID==sim_customizationscriptattribute_cleanupbeforesave) )
            thirdArgType=lua_arg_bool;


        if ( (attribID==sim_scriptattribute_executionorder)||(attribID==sim_scriptattribute_executioncount)||(attribID==sim_scriptattribute_debuglevel) )
            thirdArgType=lua_arg_number;
        int res=checkOneGeneralInputArgument(L,3,thirdArgType,0,false,false,&errorString);
        if (res==2)
        {
            if ( (attribID==sim_customizationscriptattribute_activeduringsimulation)||(attribID==sim_childscriptattribute_automaticcascadingcalls)||(attribID==sim_scriptattribute_enabled)||(attribID==sim_customizationscriptattribute_cleanupbeforesave) )
                retVal=simSetScriptAttribute_internal(scriptID,attribID,0.0f,luaToBool(L,3));
            if ( (attribID==sim_scriptattribute_executionorder)||(attribID==sim_scriptattribute_executioncount)||(attribID==sim_scriptattribute_debuglevel) )
                retVal=simSetScriptAttribute_internal(scriptID,attribID,0.0f,luaToInt(L,3));
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetScriptAttribute(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getScriptAttribute");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int scriptID=luaToInt(L,1);
        if (scriptID==sim_handle_self)
            scriptID=getCurrentScriptHandle(L);
        int attribID=luaToInt(L,2);
        int intVal;
        float floatVal;
        int result=simGetScriptAttribute_internal(scriptID,attribID,&floatVal,&intVal);
        if (result!=-1)
        {
            if ( (attribID==sim_customizationscriptattribute_activeduringsimulation)||(attribID==sim_childscriptattribute_automaticcascadingcalls)||(attribID==sim_scriptattribute_enabled)||(attribID==sim_customizationscriptattribute_cleanupbeforesave) )
                luaWrap_lua_pushboolean(L,intVal);
            if ( (attribID==sim_scriptattribute_executionorder)||(attribID==sim_scriptattribute_executioncount)||(attribID==sim_scriptattribute_scripttype) )
                luaWrap_lua_pushinteger(L,intVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simReorientShapeBoundingBox(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.reorientShapeBoundingBox");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int shapeHandle=luaToInt(L,1);
        int relativeToHandle=luaToInt(L,2);
        retVal=simReorientShapeBoundingBox_internal(shapeHandle,relativeToHandle,0);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetScriptVariable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setScriptVariable");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string varAndScriptName(luaWrap_lua_tostring(L,1));
        int scriptHandleOrType=luaWrap_lua_tointeger(L,2);
        int numberOfArguments=luaWrap_lua_gettop(L);
        if (numberOfArguments>=3)
        {
            CInterfaceStack* stack=new CInterfaceStack();
            int stackHandle=App::worldContainer->interfaceStackContainer->addStack(stack);
            stack->buildFromLuaStack(L,3,1);
            retVal=simSetScriptVariable_internal(scriptHandleOrType,varAndScriptName.c_str(),stackHandle);
            App::worldContainer->interfaceStackContainer->destroyStack(stackHandle);
        }
        else
            errorString=SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetEngineFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getEngineFloatParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        simBool ok;
        float paramVal=simGetEngineFloatParameter_internal(paramId,objectHandle,nullptr,&ok);
        if (ok>0)
        {
            luaWrap_lua_pushnumber(L,paramVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetEngineInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getEngineInt32Parameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        simBool ok;
        int paramVal=simGetEngineInt32Parameter_internal(paramId,objectHandle,nullptr,&ok);
        if (ok>0)
        {
            luaWrap_lua_pushinteger(L,paramVal);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetEngineBoolParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getEngineBoolParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        simBool ok;
        simBool paramVal=simGetEngineBoolParameter_internal(paramId,objectHandle,nullptr,&ok);
        if (ok>0)
        {
            luaWrap_lua_pushboolean(L,paramVal>0);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetEngineFloatParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setEngineFloatParameter");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        float paramVal=luaToFloat(L,3);
        retVal=simSetEngineFloatParameter_internal(paramId,objectHandle,nullptr,paramVal);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetEngineInt32Parameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setEngineInt32Parameter");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        int paramVal=luaWrap_lua_tointeger(L,3);
        retVal=simSetEngineInt32Parameter_internal(paramId,objectHandle,nullptr,paramVal);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetEngineBoolParameter(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setEngineBoolParameter");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_bool,0))
    {
        int paramId=luaWrap_lua_tointeger(L,1);
        int objectHandle=luaWrap_lua_tointeger(L,2);
        simBool paramVal=(simBool)luaWrap_lua_toboolean(L,3);
        retVal=simSetEngineBoolParameter_internal(paramId,objectHandle,nullptr,paramVal);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateOctree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createOctree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        float voxelSize=(float)luaWrap_lua_tonumber(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        float pointSize=(float)luaWrap_lua_tonumber(L,3);
        retVal=simCreateOctree_internal(voxelSize,options,pointSize,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreatePointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.createPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        float maxVoxelSize=(float)luaWrap_lua_tonumber(L,1);
        int maxPtCntPerVoxel=luaWrap_lua_tointeger(L,2);
        int options=luaWrap_lua_tointeger(L,3);
        float pointSize=(float)luaWrap_lua_tonumber(L,4);
        retVal=simCreatePointCloud_internal(maxVoxelSize,maxPtCntPerVoxel,options,pointSize,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetPointCloudOptions(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setPointCloudOptions");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float maxVoxelSize=(float)luaWrap_lua_tonumber(L,2);
        int maxPtCntPerVoxel=luaWrap_lua_tointeger(L,3);
        int options=luaWrap_lua_tointeger(L,4);
        float pointSize=(float)luaWrap_lua_tonumber(L,5);
        retVal=simSetPointCloudOptions_internal(handle,maxVoxelSize,maxPtCntPerVoxel,options,pointSize,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetPointCloudOptions(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getPointCloudOptions");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float maxVoxelSize;
        int maxPtCntPerVoxel;
        int options;
        float pointSize;
        int retVal=simGetPointCloudOptions_internal(handle,&maxVoxelSize,&maxPtCntPerVoxel,&options,&pointSize,nullptr);
        if (retVal>0)
        {
            luaWrap_lua_pushnumber(L,maxVoxelSize);
            luaWrap_lua_pushinteger(L,maxPtCntPerVoxel);
            luaWrap_lua_pushinteger(L,options);
            luaWrap_lua_pushnumber(L,pointSize);
            LUA_END(4);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simInsertVoxelsIntoOctree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.insertVoxelsIntoOctree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
        std::vector<float> pts;
        pts.resize(ptCnt*3);
        unsigned char* cols=nullptr;
        std::vector<unsigned char> _cols;
        int v=3;
        if (options&2)
            v=ptCnt*3;
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,v,true,true,&errorString);
        if (res>=0)
        {
            getFloatsFromTable(L,3,ptCnt*3,&pts[0]);
            if (res==2)
            {
                _cols.resize(v);
                getUCharsFromTable(L,4,v,&_cols[0]);
                cols=&_cols[0];
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,v/3,true,true,&errorString);
            if (res>=0)
            {
                if (cols==nullptr)
                    retVal=simInsertVoxelsIntoOctree_internal(handle,options,&pts[0],ptCnt,nullptr,nullptr,nullptr);
                else
                {
                    unsigned int* tags=nullptr;
                    std::vector<unsigned int> _tags;
                    if (res==2)
                    {
                        _tags.resize(v/3);
                        getUIntsFromTable(L,5,v/3,&_tags[0]);
                        tags=&_tags[0];
                    }
                    retVal=simInsertVoxelsIntoOctree_internal(handle,options,&pts[0],ptCnt,cols,tags,nullptr);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveVoxelsFromOctree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removeVoxelsFromOctree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,false,true,&errorString);
        if (res>=1)
        {
            if (res==2)
            { // remove some voxels
                int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
                std::vector<float> pts;
                pts.resize(ptCnt*3);
                getFloatsFromTable(L,3,ptCnt*3,&pts[0]);
                retVal=simRemoveVoxelsFromOctree_internal(handle,options,&pts[0],ptCnt,nullptr);
            }
            else
                retVal=simRemoveVoxelsFromOctree_internal(handle,options,nullptr,0,nullptr); // remove all voxels!
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simInsertPointsIntoPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.insertPointsIntoPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
        float optionalValues[2];
        ((int*)optionalValues)[0]=1; // duplicate tolerance bit
        optionalValues[1]=0.0; // duplicate tolerance
        std::vector<float> pts;
        pts.resize(ptCnt*3);
        unsigned char* cols=nullptr;
        std::vector<unsigned char> _cols;
        int v=3;
        if (options&2)
            v=ptCnt*3;
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,v,true,true,&errorString);
        if (res>=0)
        {
            getFloatsFromTable(L,3,ptCnt*3,&pts[0]);
            if (res==2)
            {
                _cols.resize(v);
                getUCharsFromTable(L,4,v,&_cols[0]);
                cols=&_cols[0];
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    optionalValues[1]=(float)luaWrap_lua_tonumber(L,5); // duplicate tolerance
                    retVal=simInsertPointsIntoPointCloud_internal(handle,options,&pts[0],ptCnt,cols,optionalValues);
                }
                else
                    retVal=simInsertPointsIntoPointCloud_internal(handle,options,&pts[0],ptCnt,cols,nullptr);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemovePointsFromPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.removePointsFromPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,false,true,&errorString);
        if (res>=1)
        {
            int res2=checkOneGeneralInputArgument(L,4,lua_arg_number,0,false,false,&errorString);
            if (res2==2)
            {
                float tolerance=(float)luaWrap_lua_tonumber(L,4);
                if (res==2)
                { // remove some points
                    int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
                    std::vector<float> pts;
                    pts.resize(ptCnt*3);
                    getFloatsFromTable(L,3,ptCnt*3,&pts[0]);
                    retVal=simRemovePointsFromPointCloud_internal(handle,options,&pts[0],ptCnt,tolerance,nullptr);
                }
                else
                    retVal=simRemovePointsFromPointCloud_internal(handle,options,nullptr,0,0.0,nullptr); // remove all points
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simIntersectPointsWithPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.intersectPointsWithPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,false,true,&errorString);
        if (res>=1)
        {
            int res2=checkOneGeneralInputArgument(L,4,lua_arg_number,0,false,false,&errorString);
            if (res2==2)
            {
                float tolerance=(float)luaWrap_lua_tonumber(L,4);
                if (res==2)
                { // intersect some points
                    int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
                    std::vector<float> pts;
                    pts.resize(ptCnt*3);
                    getFloatsFromTable(L,3,ptCnt*3,&pts[0]);
                    retVal=simIntersectPointsWithPointCloud_internal(handle,options,&pts[0],ptCnt,tolerance,nullptr);
                }
                else
                    retVal=simRemovePointsFromPointCloud_internal(handle,options,nullptr,0,0.0,nullptr); // remove all points
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetOctreeVoxels(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getOctreeVoxels");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int ptCnt=-1;
        const float* p=simGetOctreeVoxels_internal(handle,&ptCnt,nullptr);
        if (ptCnt>=0)
        {
            pushFloatTableOntoStack(L,ptCnt*3,p);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetPointCloudPoints(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getPointCloudPoints");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int ptCnt=-1;
        const float* p=simGetPointCloudPoints_internal(handle,&ptCnt,nullptr);
        if (ptCnt>=0)
        {
            pushFloatTableOntoStack(L,ptCnt*3,p);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simInsertObjectIntoOctree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.insertObjectIntoOctree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle1=luaWrap_lua_tointeger(L,1);
        int handle2=luaWrap_lua_tointeger(L,2);
        int options=luaWrap_lua_tointeger(L,3);
        unsigned char col[3];
        unsigned char* c=nullptr;
        int tag=0;
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,3,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getUCharsFromTable(L,4,3,col);
                c=col;
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,true,&errorString);
            if (res==2)
                tag=(unsigned int)luaWrap_lua_tonumber(L,5);
            retVal=simInsertObjectIntoOctree_internal(handle1,handle2,options,c,tag,nullptr);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSubtractObjectFromOctree(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.subtractObjectFromOctree");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle1=luaWrap_lua_tointeger(L,1);
        int handle2=luaWrap_lua_tointeger(L,2);
        int options=luaWrap_lua_tointeger(L,3);
        retVal=simSubtractObjectFromOctree_internal(handle1,handle2,options,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simInsertObjectIntoPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.insertObjectIntoPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle1=luaWrap_lua_tointeger(L,1);
        int handle2=luaWrap_lua_tointeger(L,2);
        int options=luaWrap_lua_tointeger(L,3);
        float gridSize=(float)luaWrap_lua_tonumber(L,4);
        float optionalValues[2];
        ((int*)optionalValues)[0]=1; // duplicate tolerance bit
        optionalValues[1]=0.0; // duplicate tolerance
        unsigned char col[3];
        unsigned char* c=nullptr;
        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,3,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getUCharsFromTable(L,5,3,col);
                c=col;
            }
            res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    optionalValues[1]=(float)luaWrap_lua_tonumber(L,6); // duplicate tolerance
                    retVal=simInsertObjectIntoPointCloud_internal(handle1,handle2,options,gridSize,c,optionalValues);
                }
                else
                    retVal=simInsertObjectIntoPointCloud_internal(handle1,handle2,options,gridSize,c,nullptr);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSubtractObjectFromPointCloud(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.subtractObjectFromPointCloud");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int handle1=luaWrap_lua_tointeger(L,1);
        int handle2=luaWrap_lua_tointeger(L,2);
        int options=luaWrap_lua_tointeger(L,3);
        float tolerance=luaWrap_lua_tonumber(L,4);
        retVal=simSubtractObjectFromPointCloud_internal(handle1,handle2,options,tolerance,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCheckOctreePointOccupancy(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.checkOctreePointOccupancy");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,3))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        int options=luaWrap_lua_tointeger(L,2);
        int ptCnt=int(luaWrap_lua_rawlen(L,3))/3;
        std::vector<float> points;
        points.resize(ptCnt*3);
        getFloatsFromTable(L,3,ptCnt*3,&points[0]);
        unsigned int tag=0;
        unsigned long long int location=0;
        retVal=simCheckOctreePointOccupancy_internal(handle,options,&points[0],ptCnt,&tag,&location,nullptr);
        if ( (retVal>0)&&(ptCnt==1) )
        { // in this case we return 4 values:
            unsigned int locLow=location&0xffffffff;
            unsigned int locHigh=(location>>32)&0xffffffff;
            luaWrap_lua_pushinteger(L,retVal);
            luaWrap_lua_pushinteger(L,tag);
            luaWrap_lua_pushinteger(L,locLow);
            luaWrap_lua_pushinteger(L,locHigh);
            LUA_END(4);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simPackTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.packTable");

    if (luaWrap_lua_gettop(L)>=1)
    {
        if (luaWrap_lua_istable(L,1))
        {
            CInterfaceStack stack;
            stack.buildFromLuaStack(L,1,1);
            std::string s(stack.getBufferFromTable());
            luaWrap_lua_pushlstring(L,s.c_str(),s.length());
            LUA_END(1);
        }
        else
            errorString.assign(SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG);
    }
    else
        errorString.assign(SIM_ERROR_FUNCTION_REQUIRES_MORE_ARGUMENTS);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simUnpackTable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.unpackTable");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        size_t l;
        const char* s=luaWrap_lua_tolstring(L,1,&l);
        CInterfaceStack stack;
        if (stack.pushTableFromBuffer(s,(unsigned int)l))
        {
            stack.buildOntoLuaStack(L,true);
            LUA_END(1);
        }
        errorString.assign(SIM_ERROR_INVALID_DATA);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleSimulationStart(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleSimulationStart");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (itScrObj->getScriptType()==sim_scripttype_mainscript)
    {
        // Following is for velocity measurement (initial):
        float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
        for (size_t i=0;i<App::currentWorld->sceneObjects->getJointCount();i++)
            App::currentWorld->sceneObjects->getJointFromIndex(i)->measureJointVelocity(dt);
        for (size_t i=0;i<App::currentWorld->sceneObjects->getObjectCount();i++)
            App::currentWorld->sceneObjects->getObjectFromIndex(i)->measureVelocity(dt);
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleSensingStart(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.handleSensingStart");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itScrObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (itScrObj->getScriptType()==sim_scripttype_mainscript)
    {
        // Following is for camera tracking!
        for (size_t i=0;i<App::currentWorld->sceneObjects->getCameraCount();i++)
        {
            CCamera*  it=App::currentWorld->sceneObjects->getCameraFromIndex(i);
            it->handleTrackingAndHeadAlwaysUp();
        }

        // Following is for velocity measurement:
        float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
        for (size_t i=0;i<App::currentWorld->sceneObjects->getJointCount();i++)
            App::currentWorld->sceneObjects->getJointFromIndex(i)->measureJointVelocity(dt);
        for (size_t i=0;i<App::currentWorld->sceneObjects->getObjectCount();i++)
            App::currentWorld->sceneObjects->getObjectFromIndex(i)->measureVelocity(dt);
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAuxFunc(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.auxFunc");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string cmd(luaWrap_lua_tostring(L,1));
        if (cmd.compare("createMirror")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,2))
            {
                float s[2];
                getFloatsFromTable(L,2,2,s);
                CMirror* m=new CMirror();
                m->setMirrorWidth(s[0]);
                m->setMirrorHeight(s[1]);
                App::currentWorld->sceneObjects->addObjectToScene(m,false,true);
                int h=m->getObjectHandle();
                luaWrap_lua_pushinteger(L,h);
                LUA_END(1);
            }
            LUA_END(0);
        }
        if (cmd.compare("activateMainWindow")==0)
        {
#ifdef SIM_WITH_GUI
            if (App::mainWindow!=nullptr)
                App::mainWindow->activateMainWindow();
#endif
            LUA_END(0);
        }
        if (cmd.compare("sleep")==0)
        {
            int res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,false,false,&errorString);
            if (res==2)
            {
                int tInMs=int(luaWrap_lua_tonumber(L,2)*1000.0);
                VThread::sleep(tInMs);
            }
            LUA_END(0);
        }
        if (cmd.compare("enableRendering")==0)
        {
            App::simThread->setRenderingAllowed(true);
            LUA_END(0);
        }
        if (cmd.compare("disableRendering")==0)
        {
            App::simThread->setRenderingAllowed(false);
            LUA_END(0);
        }
        if (cmd.compare("curveToClipboard")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
            {
                int graphHandle=luaWrap_lua_tointeger(L,2);
                int curveType=luaWrap_lua_tointeger(L,3);
                std::string curveName(luaWrap_lua_tostring(L,4));
                CGraph* it=App::currentWorld->sceneObjects->getGraphFromHandle(graphHandle);
                if (it!=nullptr)
                {
                    it->curveToClipboard(curveType,curveName.c_str());
                    LUA_END(0);
                }
                else
                    errorString=SIM_ERROR_INVALID_HANDLE;
            }
        }
        if (cmd.compare("curveToStatic")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
            {
                int graphHandle=luaWrap_lua_tointeger(L,2);
                int curveType=luaWrap_lua_tointeger(L,3);
                std::string curveName(luaWrap_lua_tostring(L,4));
                CGraph* it=App::currentWorld->sceneObjects->getGraphFromHandle(graphHandle);
                if (it!=nullptr)
                {
                    it->curveToStatic(curveType,curveName.c_str());
                    LUA_END(0);
                }
                else
                    errorString=SIM_ERROR_INVALID_HANDLE;
            }
        }
        if (cmd.compare("removeStaticCurve")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
            {
                int graphHandle=luaWrap_lua_tointeger(L,2);
                int curveType=luaWrap_lua_tointeger(L,3);
                std::string curveName(luaWrap_lua_tostring(L,4));
                CGraph* it=App::currentWorld->sceneObjects->getGraphFromHandle(graphHandle);
                if (it!=nullptr)
                {
                    it->removeStaticCurve(curveType,curveName.c_str());
                    LUA_END(0);
                }
                else
                    errorString=SIM_ERROR_INVALID_HANDLE;
            }
        }
        if (cmd.compare("setAssemblyMatchValues")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_bool,0,lua_arg_string,0))
            {
                int objHandle=luaWrap_lua_tointeger(L,2);
                bool childAttr=luaWrap_lua_toboolean(L,3);
                std::string matchValues(luaWrap_lua_tostring(L,4));
                CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(objHandle);
                if (it!=nullptr)
                {
                    it->setAssemblyMatchValues(childAttr,matchValues.c_str());
                    LUA_END(0);
                }
                else
                    errorString=SIM_ERROR_INVALID_HANDLE;
            }
        }
        if (cmd.compare("getAssemblyMatchValues")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_bool,0))
            {
                int objHandle=luaWrap_lua_tointeger(L,2);
                bool childAttr=luaWrap_lua_toboolean(L,3);
                CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(objHandle);
                if (it!=nullptr)
                {
                    std::string str(it->getAssemblyMatchValues(childAttr));
                    luaWrap_lua_pushstring(L,str.c_str());
                    LUA_END(1);
                }
                else
                    errorString=SIM_ERROR_INVALID_HANDLE;
            }
        }
#ifdef SIM_WITH_GUI
        if (cmd.compare("drawImageLines")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0,lua_arg_number,2,lua_arg_number,4,lua_arg_number,3,lua_arg_number,0))
            {
                int res[2];
                getIntsFromTable(L,3,2,res);
                size_t imgLen;
                const char* img=luaWrap_lua_tolstring(L,2,&imgLen);
                if (imgLen==size_t(3*res[0]*res[1]))
                {
                    std::vector<int> lines;
                    int vc=int(luaWrap_lua_rawlen(L,4));
                    vc=2*(vc/2);
                    lines.resize(vc);
                    getIntsFromTable(L,4,vc,&lines[0]);
                    int col[3];
                    getIntsFromTable(L,5,3,col);
                    int lineWidth=luaWrap_lua_tointeger(L,6);
                    QImage qimg((const unsigned char*)img,res[0],res[1],res[0]*3,QImage::Format_RGB888);
                    QPixmap pix(QPixmap::fromImage(qimg));
                    QPainter paint(&pix);
                    QColor qcol(col[0],col[1],col[2],255);
                    QPen pen(qcol);
                    pen.setWidth(lineWidth);
                    paint.setPen(pen);
                    for (size_t i=0;i<(lines.size()/2)-1;i++)
                        paint.drawLine(lines[2*i+0],lines[2*i+1],lines[2*i+2],lines[2*i+3]);
                    qimg=pix.toImage();
                    std::vector<unsigned char> img2;
                    img2.resize(res[0]*res[1]*3);
                    for (int y=0;y<res[1];y++)
                    {
                        for (int x=0;x<res[0];x++)
                        {
                            QRgb rgb=qimg.pixel(x,y);
                            img2[y*res[0]*3+3*x+0]=qRed(rgb);
                            img2[y*res[0]*3+3*x+1]=qGreen(rgb);
                            img2[y*res[0]*3+3*x+2]=qBlue(rgb);
                        }
                    }
                    luaWrap_lua_pushlstring(L,(const char*)&img2[0],res[0]*res[1]*3);
                    LUA_END(1);
                }
            }
        }
        if (cmd.compare("drawImageSquares")==0)
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0,lua_arg_number,2,lua_arg_number,2,lua_arg_number,3,lua_arg_number,0))
            {
                int res[2];
                getIntsFromTable(L,3,2,res);
                size_t imgLen;
                const char* img=luaWrap_lua_tolstring(L,2,&imgLen);
                if (imgLen==size_t(3*res[0]*res[1]))
                {
                    std::vector<int> rects;
                    int vc=int(luaWrap_lua_rawlen(L,4));
                    vc=4*(vc/4);
                    rects.resize(vc);
                    getIntsFromTable(L,4,vc,&rects[0]);
                    int col[3];
                    getIntsFromTable(L,5,3,col);
                    int lineWidth=luaWrap_lua_tointeger(L,6);
                    QImage qimg((const unsigned char*)img,res[0],res[1],res[0]*3,QImage::Format_RGB888);
                    QPixmap pix(QPixmap::fromImage(qimg));
                    QPainter paint(&pix);
                    QColor qcol(col[0],col[1],col[2],255);
                    QPen pen(qcol);
                    pen.setWidth(lineWidth);
                    paint.setPen(pen);
                    for (size_t i=0;i<rects.size()/4;i++)
                        paint.drawRect(rects[4*i+0],rects[4*i+1],rects[4*i+2],rects[4*i+3]);
                    qimg=pix.toImage();
                    std::vector<unsigned char> img2;
                    img2.resize(res[0]*res[1]*3);
                    for (int y=0;y<res[1];y++)
                    {
                        for (int x=0;x<res[0];x++)
                        {
                            QRgb rgb=qimg.pixel(x,y);
                            img2[y*res[0]*3+3*x+0]=qRed(rgb);
                            img2[y*res[0]*3+3*x+1]=qGreen(rgb);
                            img2[y*res[0]*3+3*x+2]=qBlue(rgb);
                        }
                    }
                    luaWrap_lua_pushlstring(L,(const char*)&img2[0],res[0]*res[1]*3);
                    LUA_END(1);
                }
            }
        }
#endif
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetReferencedHandles(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setReferencedHandles");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objHandle=(luaWrap_lua_tointeger(L,1));
        if (luaWrap_lua_istable(L,2))
        {
            int cnt=(int)luaWrap_lua_rawlen(L,2);
            if (cnt>0)
            {
                std::vector<int> handles;
                handles.resize(cnt);
                getIntsFromTable(L,2,cnt,&handles[0]);
                retVal=simSetReferencedHandles_internal(objHandle,cnt,&handles[0],nullptr,nullptr);
            }
            else
                retVal=simSetReferencedHandles_internal(objHandle,0,nullptr,nullptr,nullptr);
        }
        else
            errorString=SIM_ERROR_INVALID_ARGUMENT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetReferencedHandles(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getReferencedHandles");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int objHandle=(luaWrap_lua_tointeger(L,1));
        int* handles;
        int cnt=simGetReferencedHandles_internal(objHandle,&handles,nullptr,nullptr);
        if (cnt>=0)
        {
            pushIntTableOntoStack(L,cnt,handles);
            delete[] handles;
            LUA_END(1);
        }
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetGraphCurve(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getGraphCurve");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int graphHandle=(luaWrap_lua_tointeger(L,1));
        int graphType=(luaWrap_lua_tointeger(L,2));
        int index=(luaWrap_lua_tointeger(L,3));
        CGraph* graph=App::currentWorld->sceneObjects->getGraphFromHandle(graphHandle);
        if (graph!=nullptr)
        {
            std::vector<float> xVals;
            std::vector<float> yVals;
            std::vector<float> zVals;
            std::string label;
            int curveType;
            float col[3];
            float minMax[6];
            if (graph->getGraphCurve(graphType,index,label,xVals,yVals,zVals,curveType,col,minMax))
            {
                luaWrap_lua_pushstring(L,label.c_str());
                luaWrap_lua_pushinteger(L,curveType);
                pushFloatTableOntoStack(L,3,col);
                if (xVals.size()>0)
                    pushFloatTableOntoStack(L,(int)xVals.size(),&xVals[0]);
                else
                    pushFloatTableOntoStack(L,0,nullptr);
                if (yVals.size()>0)
                    pushFloatTableOntoStack(L,(int)yVals.size(),&yVals[0]);
                else
                    pushFloatTableOntoStack(L,0,nullptr);
                if (graphType==2)
                {
                    if (zVals.size()>0)
                        pushFloatTableOntoStack(L,(int)zVals.size(),&zVals[0]);
                    else
                        pushFloatTableOntoStack(L,0,nullptr);
                    if (xVals.size()>0)
                    {
                        pushFloatTableOntoStack(L,6,minMax);
                        LUA_END(7);
                    }
                    LUA_END(6);
                }
                else
                {
                    if (xVals.size()>0)
                    {
                        pushFloatTableOntoStack(L,4,minMax);
                        LUA_END(6);
                    }
                    LUA_END(5);
                }
            }
            else
            { // this should not generate an error!
            }
        }
        else
            errorString=SIM_ERROR_OBJECT_NOT_GRAPH;
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetGraphInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getGraphInfo");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int graphHandle=(luaWrap_lua_tointeger(L,1));
        CGraph* graph=App::currentWorld->sceneObjects->getGraphFromHandle(graphHandle);
        if (graph!=nullptr)
        {
            int bitCoded=0;
            luaWrap_lua_pushinteger(L,bitCoded);
            pushFloatTableOntoStack(L,3,graph->backgroundColor);
            pushFloatTableOntoStack(L,3,graph->textColor);
            LUA_END(3);
        }
        else
            errorString=SIM_ERROR_OBJECT_NOT_GRAPH;
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetShapeViz(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getShapeViz");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int shapeHandle=luaWrap_lua_tointeger(L,1);
        int index=luaWrap_lua_tointeger(L,2);
        SShapeVizInfo info;
        int ret=simGetShapeViz_internal(shapeHandle,index,&info);
        if (ret>0)
        {
            CInterfaceStack stack;
            stack.pushTableOntoStack();

            stack.pushStringOntoStack("vertices",0);
            stack.pushFloatArrayTableOntoStack(info.vertices,info.verticesSize);
            stack.insertDataIntoStackTable();
            stack.pushStringOntoStack("indices",0);
            stack.pushIntArrayTableOntoStack(info.indices,info.indicesSize);
            stack.insertDataIntoStackTable();
            stack.pushStringOntoStack("normals",0);
            stack.pushFloatArrayTableOntoStack(info.normals,info.indicesSize*3);
            stack.insertDataIntoStackTable();
            stack.pushStringOntoStack("colors",0);
            stack.pushFloatArrayTableOntoStack(info.colors,9);
            stack.insertDataIntoStackTable();
            stack.pushStringOntoStack("shadingAngle",0);
            stack.pushNumberOntoStack(double(info.shadingAngle));
            stack.insertDataIntoStackTable();
            delete[] info.vertices;
            delete[] info.indices;
            delete[] info.normals;
            if (ret>1)
            {
                stack.pushStringOntoStack("texture",0);
                stack.pushTableOntoStack();

                stack.pushStringOntoStack("texture",0);
                stack.pushStringOntoStack(info.texture,4*info.textureRes[0]*info.textureRes[1]);
                stack.insertDataIntoStackTable();
                stack.pushStringOntoStack("resolution",0);
                stack.pushIntArrayTableOntoStack(info.textureRes,2);
                stack.insertDataIntoStackTable();
                stack.pushStringOntoStack("coordinates",0);
                stack.pushFloatArrayTableOntoStack(info.textureCoords,info.indicesSize*2);
                stack.insertDataIntoStackTable();
                stack.pushStringOntoStack("applyMode",0);
                stack.pushNumberOntoStack(double(info.textureApplyMode));
                stack.insertDataIntoStackTable();
                stack.pushStringOntoStack("options",0);
                stack.pushNumberOntoStack(double(info.textureOptions));
                stack.insertDataIntoStackTable();
                stack.pushStringOntoStack("id",0);
                stack.pushNumberOntoStack(double(info.textureId));
                stack.insertDataIntoStackTable();

                stack.insertDataIntoStackTable();
                delete[] info.texture;
                delete[] info.textureCoords;
            }
            stack.buildOntoLuaStack(L,true);
            LUA_END(1);
        }
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simExecuteScriptString(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.executeScriptString");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string strAndScriptName(luaWrap_lua_tostring(L,1));
        int scriptHandleOrType=luaWrap_lua_tointeger(L,2);
        CInterfaceStack* stack=new CInterfaceStack();
        int stackHandle=App::worldContainer->interfaceStackContainer->addStack(stack);
        int retVal=simExecuteScriptString_internal(scriptHandleOrType,strAndScriptName.c_str(),stackHandle);
        if (retVal>=0)
        {
            luaWrap_lua_pushinteger(L,retVal);
            int s=1;
            if (stack->getStackSize()>0)
            {
                stack->buildOntoLuaStack(L,false);//true);
                s+=stack->getStackSize();
            }
            App::worldContainer->interfaceStackContainer->destroyStack(stackHandle);
            LUA_END(s);
        }
        else
        {
//            if (errorString.size()==0)
//                errorString=SIM_ERROR_OPERATION_FAILED;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simGetApiFunc(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getApiFunc");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int scriptHandleOrType=luaWrap_lua_tointeger(L,1);
        std::string apiWord(luaWrap_lua_tostring(L,2));
        char* str=simGetApiFunc_internal(scriptHandleOrType,apiWord.c_str());
        std::vector<std::string> strTable;
        if (str!=nullptr)
        {
            std::string sstr(str);
            simReleaseBuffer_internal(str);
            size_t prevPos=0;
            size_t spacePos=sstr.find(' ',prevPos);
            while (spacePos!=std::string::npos)
            {
                strTable.push_back(std::string(sstr.begin()+prevPos,sstr.begin()+spacePos));
                prevPos=spacePos+1;
                spacePos=sstr.find(' ',prevPos);
            }
            strTable.push_back(std::string(sstr.begin()+prevPos,sstr.end()));
        }
        pushStringTableOntoStack(L,strTable);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetApiInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getApiInfo");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int scriptHandleOrType=luaWrap_lua_tointeger(L,1);
        std::string apiWord(luaWrap_lua_tostring(L,2));
        char* str=simGetApiInfo_internal(scriptHandleOrType,apiWord.c_str());
        if (str!=nullptr)
        {
            luaWrap_lua_pushstring(L,str);
            simReleaseBuffer_internal(str);
            LUA_END(1);
        }
        LUA_END(0);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetModuleInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getModuleInfo");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        char* stringInfo;
        int intInfo;
        std::string moduleName(luaWrap_lua_tostring(L,1));
        int infoType=luaWrap_lua_tointeger(L,2);
        int res=simGetModuleInfo_internal(moduleName.c_str(),infoType,&stringInfo,&intInfo);
        if (res>=0)
        {
            if ( (infoType==sim_moduleinfo_extversionstr)||(infoType==sim_moduleinfo_builddatestr) )
            {
                luaWrap_lua_pushstring(L,stringInfo);
                delete[] stringInfo;
                LUA_END(1);
            }
            if ( (infoType==sim_moduleinfo_extversionint)||(infoType==sim_moduleinfo_verbosity)||(infoType==sim_moduleinfo_statusbarverbosity) )
            {
                luaWrap_lua_pushinteger(L,intInfo);
                LUA_END(1);
            }
        }
        LUA_END(0);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetModuleInfo(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.setModuleInfo");

    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0))
    {
        std::string moduleName(luaWrap_lua_tostring(L,1));
        int infoType=luaWrap_lua_tointeger(L,2);
        if ( (infoType==sim_moduleinfo_verbosity)||(infoType==sim_moduleinfo_statusbarverbosity) )
        {
            int res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,false,false,&errorString);
            if (res==2)
            {
                int verbosity=luaWrap_lua_tointeger(L,3);
                simSetModuleInfo_internal(moduleName.c_str(),infoType,nullptr,verbosity);
            }
            else
                errorString=SIM_ERROR_ONE_ARGUMENT_TYPE_IS_WRONG;
        }
        else
            errorString=SIM_ERROR_INVALID_MODULE_INFO_TYPE;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRegisterScriptFunction(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.registerScriptFunction");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_string,0))
    {
        std::string funcNameAtPluginName(luaWrap_lua_tostring(L,1));
        std::string ct(luaWrap_lua_tostring(L,2));
        ct="####"+ct;
        retVal=simRegisterScriptCallbackFunction_internal(funcNameAtPluginName.c_str(),ct.c_str(),nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRegisterScriptVariable(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.registerScriptVariable");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        const char* varNameAtPluginName=luaWrap_lua_tostring(L,1);
        if (checkOneGeneralInputArgument(L,2,lua_arg_string,0,true,false,&errorString)==2)
            retVal=simRegisterScriptVariable_internal(varNameAtPluginName,luaWrap_lua_tostring(L,2),-1);
        else
            retVal=simRegisterScriptVariable_internal(varNameAtPluginName,nullptr,0);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simIsDeprecated(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.isDeprecated");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
        retVal=simIsDeprecated_internal(luaWrap_lua_tostring(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetPersistentDataTags(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getPersistentDataTags");

    int tagCount;
    char* data=simGetPersistentDataTags_internal(&tagCount);
    if (data!=nullptr)
    {
        std::vector<std::string> stringTable;
        size_t off=0;
        for (int i=0;i<tagCount;i++)
        {
            stringTable.push_back(data+off);
            off+=strlen(data+off)+1;
        }
        pushStringTableOntoStack(L,stringTable);
        simReleaseBuffer_internal(data);
        LUA_END(1);
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetRandom(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.getRandom");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (it!=nullptr)
    {
        int res=checkOneGeneralInputArgument(L,1,lua_arg_number,0,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                unsigned int s=abs(luaToInt(L,1));
                it->setRandomSeed(s);
            }
            luaWrap_lua_pushnumber(L,it->getRandomDouble());
            LUA_END(1);
        }
    }
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

//****************************************************
//****************************************************
//****************************************************
//****************************************************
//****************************************************
//****************************************************

int _simResetMill(luaWrap_lua_State* L)
{ // DEPRECATED since V4.0.1. has no effect anymore
    LUA_START("sim.ResetMill");
    LUA_END(0);
}

int _simHandleMill(luaWrap_lua_State* L)
{ // DEPRECATED since V4.0.1. has no effect anymore
    LUA_START("sim.HandleMill");
    LUA_END(0);
}

int _simResetMilling(luaWrap_lua_State* L)
{ // DEPRECATED since V4.0.1. has no effect anymore
    LUA_START("sim.ResetMilling");
    LUA_END(0);
}


int _simOpenTextEditor(luaWrap_lua_State* L)
{ // DEPRECATED since V3.6.0
    TRACE_LUA_API;
    LUA_START("sim.openTextEditor");

    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string initText(luaWrap_lua_tostring(L,1));
        int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,true,true,&errorString);
        if (res>=0)
        {
            std::string xml;
            const char* _xml=nullptr;
            if (res==2)
            {
                xml=luaWrap_lua_tostring(L,2);
                _xml=xml.c_str();
            }
            int res=checkOneGeneralInputArgument(L,3,lua_arg_string,0,true,true,&errorString);
            if (res!=2)
            { // Modal dlg
                int various[4];
                char* outText=simOpenTextEditor_internal(initText.c_str(),_xml,various);
                if (outText!=nullptr)
                {
                    luaWrap_lua_pushstring(L,outText);
                    delete[] outText;
                    pushIntTableOntoStack(L,2,various+0);
                    pushIntTableOntoStack(L,2,various+2);
                    LUA_END(3);
                }
            }
            else
            { // non-modal dlg
                int handle=-1;
                CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
                if (it!=nullptr)
                {
                    std::string callbackFunction(luaWrap_lua_tostring(L,3));
#ifdef SIM_WITH_GUI
                    if (App::mainWindow!=nullptr)
                        handle=App::mainWindow->codeEditorContainer->openTextEditor(initText.c_str(),xml.c_str(),callbackFunction.c_str(),it->getScriptID(),it->isSimulationScript());
#endif
                }
                luaWrap_lua_pushinteger(L,handle);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCloseTextEditor(luaWrap_lua_State* L)
{ // DEPRECATED since V3.6.0
    TRACE_LUA_API;
    LUA_START("sim.closeTextEditor");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0))
    {
        int h=luaWrap_lua_tointeger(L,1);
        bool ignoreCb=luaWrap_lua_toboolean(L,2);
        int res=0;
        std::string txt;
        std::string cb;
        int posAndSize[4];
#ifdef SIM_WITH_GUI
        if (App::mainWindow!=nullptr)
        {
            if (App::mainWindow->codeEditorContainer->close(h,posAndSize,&txt,&cb))
                res=1;
        }
#endif
        if ( (res>0)&&(!ignoreCb) )
        {   // We call the callback directly from here:
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
            if (it!=nullptr)
            {
                CInterfaceStack stack;
                stack.pushStringOntoStack(txt.c_str(),int(txt.size()));
                stack.pushIntArrayTableOntoStack(posAndSize+0,2);
                stack.pushIntArrayTableOntoStack(posAndSize+2,2);
                it->callScriptFunctionEx(cb.c_str(),&stack);
            }
        }
        luaWrap_lua_pushinteger(L,res);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleVarious(luaWrap_lua_State* L)
{ // DEPRECATED since V3.4.0
    TRACE_LUA_API;
    LUA_START("simHandleVarious");

    int retVal=simHandleVarious_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetMpConfigForTipPose(luaWrap_lua_State* L)
{ // DEPRECATED since V3.3.0
    TRACE_LUA_API;
    LUA_START("simGetMpConfigForTipPose");
    errorString="Not supported anymore.";
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}


int _simResetPath(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simResetPath");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetPath_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandlePath(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simHandlePath");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simHandlePath_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetJoint(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simResetJoint");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simResetJoint_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleJoint(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simHandleJoint");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simHandleJoint_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResetTracing(luaWrap_lua_State* L)
{ // deprecated
    TRACE_LUA_API;
    LUA_START("simResetTracing");

    warningString=SIM_ERROR_FUNCTION_DEPRECATED_AND_HAS_NO_EFFECT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleTracing(luaWrap_lua_State* L)
{ // deprecated
    TRACE_LUA_API;
    LUA_START("simHandleTracing");

    warningString=SIM_ERROR_FUNCTION_DEPRECATED_AND_HAS_NO_EFFECT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simMoveToPosition(luaWrap_lua_State* L)
{ // can only be called from a script running in a thread!! DEPRECATED
    TRACE_LUA_API;
    LUA_START("simMoveToPosition");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        { // Those are the arguments that are always required! (the rest can be ignored or set to nil!
            int objID=luaWrap_lua_tointeger(L,1);
            int relativeToObjID=luaWrap_lua_tointeger(L,2);
            float posTarget[3];
            float eulerTarget[3];
            float maxVelocity;
            CSceneObject* object=App::currentWorld->sceneObjects->getObjectFromHandle(objID);
            CSceneObject* relToObject=nullptr;
            float accel=0.0f; // means infinite accel!! (default value)
            float angleToLinearCoeff=0.1f/(90.0f*degToRad_f); // (default value)
            int distCalcMethod=sim_distcalcmethod_dl_if_nonzero; // (default value)
            bool foundError=false;
            if ((!foundError)&&(object==nullptr))
            {
                errorString=SIM_ERROR_OBJECT_INEXISTANT;
                foundError=true;
            }
            if ((!foundError)&&(relativeToObjID==sim_handle_parent))
            {
                relativeToObjID=-1;
                CSceneObject* parent=object->getParent();
                if (parent!=nullptr)
                    relativeToObjID=parent->getObjectHandle();
            }
            if ((!foundError)&&(relativeToObjID!=-1))
            {
                relToObject=App::currentWorld->sceneObjects->getObjectFromHandle(relativeToObjID);
                if (relToObject==nullptr)
                { // error, object doesn't exist!
                    errorString=SIM_ERROR_OBJECT_INEXISTANT;
                    foundError=true;
                }
            }

            // Now check the optional arguments:
            int res;
            unsigned char posAndOrient=0;
            if (!foundError) // position argument:
            {
                res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,true,true,&errorString);
                if (res==2)
                { // get the data
                    getFloatsFromTable(L,3,3,posTarget);
                    posAndOrient|=1;
                }
                foundError=(res==-1);
            }
            if (!foundError) // orientation argument:
            {
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,3,true,true,&errorString);
                if (res==2)
                { // get the data
                    getFloatsFromTable(L,4,3,eulerTarget);
                    posAndOrient|=2;
                }
                foundError=(res==-1);
            }
            if ((!foundError)&&(posAndOrient==0))
            {
                foundError=true;
                errorString="Target position and/or target orientation has to be specified.";
            }
            if (!foundError) // target velocity argument:
            {
                res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,false,false,&errorString);
                if (res==2)
                { // get the data
                    maxVelocity=luaToFloat(L,5);
                }
                else
                    foundError=true;
            }
            if (!foundError) // Accel argument:
            {
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,true,&errorString);
                if (res==2)
                { // get the data
                    accel=fabs(luaToFloat(L,6));
                }
                foundError=(res==-1);
            }
            if (!foundError) // distance method:
            {
                res=checkOneGeneralInputArgument(L,7,lua_arg_number,2,true,true,&errorString);
                if (res==2)
                { // get the data
                    float tmpF[2];
                    getFloatsFromTable(L,7,2,tmpF);
                    angleToLinearCoeff=tmpF[1];
                    getIntsFromTable(L,7,1,&distCalcMethod);
                }
                foundError=(res==-1);
            }
            /*
              Crashes the compiler:
            if (!foundError) // distance method:
            {
                res=checkOneGeneralInputArgument(L,7,lua_arg_number,2,true,true,&errorString);
                if (res==2)
                { // get the data
                    float tmpF[2];
                    int tmpI[2];
                    getFloatsFromTable(L,7,2,tmpF);
                    getIntsFromTable(L,7,2,tmpI);
                    distCalcMethod=tmpI[0];
                    angleToLinearCoeff=tmpF[1];
                }
                foundError=(res==-1);
            }
            */
            if (!foundError)
            { // do the job here!
                C7Vector startTr(object->getCumulativeTransformation());
                C7Vector relTr;
                relTr.setIdentity();
                if (relToObject!=nullptr)
                    relTr=relToObject->getFullCumulativeTransformation();
                startTr=relTr.getInverse()*startTr;

                C7Vector targetTr(startTr);
                if (posAndOrient&1)
                    targetTr.X.set(posTarget);
                if (posAndOrient&2)
                    targetTr.Q.setEulerAngles(eulerTarget[0],eulerTarget[1],eulerTarget[2]);
                float currentVel=0.0f;
                CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                float lastTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+threadData->usedMovementTime;

                float dl=(targetTr.X-startTr.X).getLength();
                float da=targetTr.Q.getAngleBetweenQuaternions(startTr.Q)*angleToLinearCoeff;
                float vdl=dl;
                if (distCalcMethod==sim_distcalcmethod_dl)
                    vdl=dl;
                if (distCalcMethod==sim_distcalcmethod_dac)
                    vdl=da;
                if (distCalcMethod==sim_distcalcmethod_max_dl_dac)
                    vdl=std::max<float>(dl,da);
                if (distCalcMethod==sim_distcalcmethod_dl_and_dac)
                    vdl=dl+da;
                if (distCalcMethod==sim_distcalcmethod_sqrt_dl2_and_dac2)
                    vdl=sqrtf(dl*dl+da*da);
                if (distCalcMethod==sim_distcalcmethod_dl_if_nonzero)
                {
                    vdl=dl;
                    if (dl<0.00005f) // Was dl==0.0f before (tolerance problem). Changed on 1/4/2011
                        vdl=da;
                }
                if (distCalcMethod==sim_distcalcmethod_dac_if_nonzero)
                {
                    vdl=da;
                    if (da<0.01f*degToRad_f) // Was da==0.0f before (tolerance problem). Changed on 1/4/2011
                        vdl=dl;
                }
                // vld is the totalvirtual distance
                float currentPos=0.0f;
                bool movementFinished=false;
                float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f; // this is the time left if we leave here

                if (vdl==0.0f)
                { // if the path length is 0 (the two positions might still be not-coincident, depending on the calculation method!)
                    if (App::currentWorld->sceneObjects->getObjectFromHandle(objID)==object) // make sure the object is still valid (running in a thread)
                    {
                        if (relToObject==nullptr)
                        { // absolute
                            C7Vector parentInv(object->getFullParentCumulativeTransformation().getInverse());
                            object->setLocalTransformation(parentInv*targetTr);
                        }
                        else
                        { // relative to a specific object (2009/11/17)
                            if (App::currentWorld->sceneObjects->getObjectFromHandle(relativeToObjID)==relToObject) // make sure the object is still valid (running in a thread)
                            { // ok
                                C7Vector relToTr(relToObject->getFullCumulativeTransformation());
                                targetTr=relToTr*targetTr;
                                C7Vector parentInv(object->getFullParentCumulativeTransformation().getInverse());
                                object->setLocalTransformation(parentInv*targetTr);
                            }
                        }
                    }
                    movementFinished=true;
                }

                bool err=false;
                while (!movementFinished)
                {
                    float currentTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
                    dt=currentTime-lastTime;
                    lastTime=currentTime;

                    if (accel==0.0f)
                    { // Means infinite acceleration
                        float timeNeeded=(vdl-currentPos)/maxVelocity;
                        currentVel=maxVelocity;
                        if (timeNeeded>dt)
                        {
                            currentPos+=dt*maxVelocity;
                            dt=0.0f; // this is what is left
                        }
                        else
                        {
                            currentPos=vdl;
                            if (timeNeeded>=0.0f)
                                dt-=timeNeeded;
                        }
                    }
                    else
                    {
                        double p=currentPos;
                        double v=currentVel;
                        double t=dt;
                        CLinMotionRoutines::getNextValues(p,v,maxVelocity,accel,0.0f,vdl,0.0f,0.0f,t);
                        currentPos=float(p);
                        currentVel=float(v);
                        dt=float(t);
                    }

                    // Now check if we are within tolerances:
                    if (fabs(currentPos-vdl)<=0.00001f)//tol[0])
                        movementFinished=true;

                    // Set the new configuration of the object:
                    float ll=currentPos/vdl;
                    if (ll>1.0f)
                        ll=1.0f;
                    C7Vector newAbs;
                    newAbs.buildInterpolation(startTr,targetTr,ll);
                    if (App::currentWorld->sceneObjects->getObjectFromHandle(objID)==object) // make sure the object is still valid (running in a thread)
                    {
                        if ( (relToObject!=nullptr)&&(App::currentWorld->sceneObjects->getObjectFromHandle(relativeToObjID)!=relToObject) )
                            movementFinished=true; // the object was destroyed during execution of the command!
                        else
                        {
                            C7Vector parentInv(object->getFullParentCumulativeTransformation().getInverse());
                            C7Vector currAbs(object->getCumulativeTransformation());
                            C7Vector relToTr;
                            relToTr.setIdentity();
                            if (relToObject!=nullptr)
                                relToTr=relToObject->getFullCumulativeTransformation();
                            currAbs=relToTr.getInverse()*currAbs;
                            if ((posAndOrient&1)==0)
                                newAbs.X=currAbs.X;
                            if ((posAndOrient&2)==0)
                                newAbs.Q=currAbs.Q;
                            newAbs=relToTr*newAbs;
                            object->setLocalTransformation(parentInv*newAbs);
                        }
                    }
                    else
                        movementFinished=true; // the object was destroyed during execution of the command!

                    if (!movementFinished)
                    {
                        CThreadPool::switchBackToPreviousThread();
                        if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                        {
                            errorString="@yield"; // yield will be triggered at end of this function
                            bool err=true;
                            break; // error
                        }
                    }
                }
                if (!err)
                {
                    // The movement finished. Now add the time used:
                    threadData->usedMovementTime=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f-dt;
                    luaWrap_lua_pushnumber(L,dt); // success (deltaTime left)
                    LUA_END(1);
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simMoveToJointPositions(luaWrap_lua_State* L)
{ // can only be called from a script running in a thread!! DEPRECATED
    TRACE_LUA_API;
    LUA_START("simMoveToJointPositions");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (!(CThreadPool::getSimulationStopRequested()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L))))
        { // Important to first check if we are supposed to leave the thread!!
            if (!( (!luaWrap_lua_istable(L,1))||(!luaWrap_lua_istable(L,2))||(luaWrap_lua_rawlen(L,1)>luaWrap_lua_rawlen(L,2))||(luaWrap_lua_rawlen(L,1)==0) ))
            { // Ok we have 2 tables with same sizes.
                int tableLen=(int)luaWrap_lua_rawlen(L,1);
                bool sameTimeFinish=true;
                float maxVelocity=0.0f;
                float accel=0.0f; // means infinite accel!! (default value)
                bool accelTablePresent=false;
                float angleToLinearCoeff=1.0f;
                bool foundError=false;
                // Now check the other arguments:
                int res;
                if (luaWrap_lua_istable(L,3))
                    sameTimeFinish=false; // we do not finish at the same time!
                if (!foundError) // velocity or velocities argument (not optional!):
                {
                    if (sameTimeFinish)
                    {
                        res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,false,false,&errorString);
                        if (res==2)
                            maxVelocity=luaToFloat(L,3);
                        else
                            foundError=true;
                    }
                    else
                    {
                        res=checkOneGeneralInputArgument(L,3,lua_arg_number,tableLen,false,false,&errorString);
                        if (res!=2)
                            foundError=true;
                    }
                }
                if (!foundError) // Accel argument:
                {
                    if (sameTimeFinish)
                    {
                        res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,true,&errorString);
                        if (res==2)
                        { // get the data
                            accel=fabs(luaToFloat(L,4));
                        }
                        foundError=(res==-1);
                    }
                    else
                    {
                        res=checkOneGeneralInputArgument(L,4,lua_arg_number,tableLen,true,true,&errorString);
                        if (res==2)
                            accelTablePresent=true;
                        foundError=(res==-1);
                    }
                }
                if (!foundError) // angleToLinearCoeff argument:
                {
                    if (sameTimeFinish)
                    {
                        res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,true,&errorString);
                        if (res==2)
                        { // get the data
                            angleToLinearCoeff=fabs(luaToFloat(L,5));
                        }
                        foundError=(res==-1);
                    }
                    else
                        angleToLinearCoeff=1.0f; // no conversion!
                }
                if (!foundError)
                { // do the job here!
                    int* jointHandles=new int[tableLen];
                    float* jointCurrentVirtualPositions=new float[tableLen];
                    float* jointCurrentVirtualVelocities=new float[tableLen];
                    float* jointStartPositions=new float[tableLen];
                    float* jointTargetPositions=new float[tableLen];
                    float* jointVirtualDistances=new float[tableLen];
                    float* jointMaxVelocities=new float[tableLen];
                    float* jointAccels=new float[tableLen];

                    getIntsFromTable(L,1,tableLen,jointHandles);
                    getFloatsFromTable(L,2,tableLen,jointTargetPositions);
                    if (!sameTimeFinish)
                    {
                        getFloatsFromTable(L,3,tableLen,jointMaxVelocities);
                        for (int i=0;i<tableLen;i++)
                            jointMaxVelocities[i]=fabs(jointMaxVelocities[i]);
                        if (accelTablePresent)
                        {
                            getFloatsFromTable(L,4,tableLen,jointAccels);
                            for (int i=0;i<tableLen;i++)
                                jointAccels[i]=fabs(jointAccels[i]);
                        }
                    }
                    float maxVirtualDist=0.0f;
                    int maxVirtualDistIndex=0;
                    for (int i=0;i<tableLen;i++)
                    {
                        jointCurrentVirtualPositions[i]=0.0f;
                        jointCurrentVirtualVelocities[i]=0.0f;
                        if (sameTimeFinish)
                            jointMaxVelocities[i]=maxVelocity;
                        if (!accelTablePresent)
                            jointAccels[i]=accel;

                        CJoint* it=App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i]);
                        if ((it!=nullptr)&&(it->getJointType()!=sim_joint_spherical_subtype))
                        { // make sure target is within allowed range, and check the maximum virtual distance:
                            jointStartPositions[i]=it->getPosition();
                            float minP=it->getPositionIntervalMin();
                            float maxP=minP+it->getPositionIntervalRange();
                            if (it->getPositionIsCyclic())
                            {
                                float da=tt::getAngleMinusAlpha(jointTargetPositions[i],jointStartPositions[i]);
                                jointTargetPositions[i]=jointStartPositions[i]+da;
                            }
                            else
                            {
                                if (minP>jointTargetPositions[i])
                                    jointTargetPositions[i]=minP;
                                if (maxP<jointTargetPositions[i])
                                    jointTargetPositions[i]=maxP;
                            }
                            float d=fabs(jointTargetPositions[i]-jointStartPositions[i]);
                            if (it->getJointType()==sim_joint_revolute_subtype)
                                d*=angleToLinearCoeff;
                            jointVirtualDistances[i]=d;
                            if (d>maxVirtualDist)
                            {
                                maxVirtualDist=d;
                                maxVirtualDistIndex=i;
                            }
                        }
                        else
                        {
                            // Following are default values in case the joint doesn't exist or is spherical:
                            jointStartPositions[i]=0.0f;
                            jointTargetPositions[i]=0.0f;
                            jointVirtualDistances[i]=0.0f;
                        }
                    }
                    CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                    float lastTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+threadData->usedMovementTime;
                    bool movementFinished=false;
                    float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f; // this is the time left if we leave here

                    if (maxVirtualDist==0.0f)
                        movementFinished=true;
                    bool err=true;
                    while (!movementFinished)
                    {
                        float currentTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
                        dt=currentTime-lastTime;
                        float minTimeLeft=dt;
                        lastTime=currentTime;
                        if (sameTimeFinish)
                        {
                            float timeLeftLocal=dt;
                            // 1. handle the joint with longest distance first:
                            // Does the main joint still exist?
                            if (App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[maxVirtualDistIndex])!=nullptr)
                            {
                                if (accel==0.0f)
                                { // means infinite accel
                                    float timeNeeded=(jointVirtualDistances[maxVirtualDistIndex]-jointCurrentVirtualPositions[maxVirtualDistIndex])/maxVelocity;
                                    jointCurrentVirtualVelocities[maxVirtualDistIndex]=maxVelocity;
                                    if (timeNeeded>timeLeftLocal)
                                    {
                                        jointCurrentVirtualPositions[maxVirtualDistIndex]+=timeLeftLocal*maxVelocity;
                                        timeLeftLocal=0.0f; // this is what is left
                                    }
                                    else
                                    {
                                        jointCurrentVirtualPositions[maxVirtualDistIndex]=jointVirtualDistances[maxVirtualDistIndex];
                                        if (timeNeeded>=0.0f)
                                            timeLeftLocal-=timeNeeded;
                                    }
                                }
                                else
                                {
                                    double p=jointCurrentVirtualPositions[maxVirtualDistIndex];
                                    double v=jointCurrentVirtualVelocities[maxVirtualDistIndex];
                                    double t=timeLeftLocal;
                                    CLinMotionRoutines::getNextValues(p,v,maxVelocity,accel,0.0f,jointVirtualDistances[maxVirtualDistIndex],0.0f,0.0f,t);
                                    jointCurrentVirtualPositions[maxVirtualDistIndex]=float(p);
                                    jointCurrentVirtualVelocities[maxVirtualDistIndex]=float(v);
                                    timeLeftLocal=float(t);
                                }
                                minTimeLeft=timeLeftLocal;
                                // 2. We adjust the other joints accordingly:
                                float f=1;
                                if (jointVirtualDistances[maxVirtualDistIndex]!=0.0f)
                                    f=jointCurrentVirtualPositions[maxVirtualDistIndex]/jointVirtualDistances[maxVirtualDistIndex];
                                for (int i=0;i<tableLen;i++)
                                {
                                    if (i!=maxVirtualDistIndex)
                                        jointCurrentVirtualPositions[i]=jointVirtualDistances[i]*f;
                                }
                                // 3. Check if within tolerance:
                                if (fabs(jointCurrentVirtualPositions[maxVirtualDistIndex]-jointVirtualDistances[maxVirtualDistIndex])<=0.00001f)
                                    movementFinished=true;
                            }
                            else
                            { // the main joint was removed. End here!
                                movementFinished=true;
                            }
                        }
                        else
                        {
                            bool withinTolerance=true;
                            for (int i=0;i<tableLen;i++)
                            {
                                if (App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i])!=nullptr)
                                {
                                    // Check if within tolerance (before):
                                    if (fabs(jointCurrentVirtualPositions[i]-jointVirtualDistances[i])>0.00001f)
                                    {
                                        float timeLeftLocal=dt;
                                        if (jointAccels[i]==0.0f)
                                        { // means infinite accel
                                            float timeNeeded=(jointVirtualDistances[i]-jointCurrentVirtualPositions[i])/jointMaxVelocities[i];
                                            jointCurrentVirtualVelocities[i]=jointMaxVelocities[i];
                                            if (timeNeeded>timeLeftLocal)
                                            {
                                                jointCurrentVirtualPositions[i]+=timeLeftLocal*jointMaxVelocities[i];
                                                timeLeftLocal=0.0f; // this is what is left
                                            }
                                            else
                                            {
                                                jointCurrentVirtualPositions[i]=jointVirtualDistances[i];
                                                if (timeNeeded>=0.0f)
                                                    timeLeftLocal-=timeNeeded;
                                            }
                                        }
                                        else
                                        {
                                            double p=jointCurrentVirtualPositions[i];
                                            double v=jointCurrentVirtualVelocities[i];
                                            double t=timeLeftLocal;
                                            CLinMotionRoutines::getNextValues(p,v,jointMaxVelocities[i],jointAccels[i],0.0f,jointVirtualDistances[i],0.0f,0.0f,t);
                                            jointCurrentVirtualPositions[i]=float(p);
                                            jointCurrentVirtualVelocities[i]=float(v);
                                            timeLeftLocal=float(t);
                                        }
                                        if (timeLeftLocal<minTimeLeft)
                                            minTimeLeft=timeLeftLocal;
                                        // Check if within tolerance (after):
                                        if (fabs(jointCurrentVirtualPositions[i]-jointVirtualDistances[i])>0.00001f)
                                            withinTolerance=false;
                                    }
                                }
                            }
                            if (withinTolerance)
                                movementFinished=true;
                        }
                        dt=minTimeLeft;

                        // We set all joint positions:
                        for (int i=0;i<tableLen;i++)
                        {
                            CJoint* joint=App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i]);
                            if ( (joint!=nullptr)&&(joint->getJointType()!=sim_joint_spherical_subtype)&&(jointVirtualDistances[i]!=0.0f) )
                            {
                                if (joint->getJointMode()==sim_jointmode_force)
                                    joint->setDynamicMotorPositionControlTargetPosition(jointStartPositions[i]+(jointTargetPositions[i]-jointStartPositions[i])*jointCurrentVirtualPositions[i]/jointVirtualDistances[i]);
                                else
                                    joint->setPosition(jointStartPositions[i]+(jointTargetPositions[i]-jointStartPositions[i])*jointCurrentVirtualPositions[i]/jointVirtualDistances[i]);
                            }
                        }

                        if (!movementFinished)
                        {
                            CThreadPool::switchBackToPreviousThread();
                            if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                            {
                                errorString="@yield"; // yield will be triggered at end of this function
                                err=true;
                                break; // error
                            }
                        }
                    }
                    // The movement finished. Now add the time used:
                    threadData->usedMovementTime=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f-dt;
                    if (!err)
                        luaWrap_lua_pushnumber(L,dt); // success (deltaTime left)

                    delete[] jointAccels;
                    delete[] jointMaxVelocities;
                    delete[] jointVirtualDistances;
                    delete[] jointTargetPositions;
                    delete[] jointStartPositions;
                    delete[] jointCurrentVirtualVelocities;
                    delete[] jointCurrentVirtualPositions;
                    delete[] jointHandles;
                    if (!err)
                        LUA_END(1);
                }
            }
            else
                errorString="One of the function's argument type is not correct or table sizes are invalid or do not match";
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetInstanceIndex(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,0);
    LUA_END(1);
}

int _simGetVisibleInstanceIndex(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,0);
    LUA_END(1);
}

int _simGetSystemTimeInMilliseconds(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetSystemTimeInMilliseconds");

    int res=checkOneGeneralInputArgument(L,1,lua_arg_number,0,true,false,&errorString);
    if (res==0)
    {
        luaWrap_lua_pushinteger(L,VDateTime::getTimeInMs());
        LUA_END(1);
    }
    if (res==2)
    {
        int lastTime=luaToInt(L,1);
        luaWrap_lua_pushinteger(L,VDateTime::getTimeDiffInMs(lastTime));
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simLockInterface(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simLockInterface");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}

int _simJointGetForce(luaWrap_lua_State* L)
{ // DEPRECATED since release 3.1.2
    TRACE_LUA_API;
    LUA_START("simJointGetForce");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float jointF[1];
        if (simGetJointForce_internal(luaToInt(L,1),jointF)>0)
        {
            luaWrap_lua_pushnumber(L,jointF[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRMLPosition(luaWrap_lua_State* L)
{ // DEPRECATED since release 3.1.2
    TRACE_LUA_API;
    LUA_START_NO_CSIDE_ERROR("simRMLPosition");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* scr=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (!scr->checkAndSetWarning_simRMLPosition_oldCompatibility_30_8_2014())
        warningString="Function is deprecated. Use simRMLPos instead.";

    int retVal=-43; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int dofs=luaToInt(L,1);
        double timeStep=luaWrap_lua_tonumber(L,2);
        int flags=luaToInt(L,3);
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,dofs*3,false,false,&errorString);
        if (res==2)
        {
            double* currentPosVelAccel=new double[dofs*3];
            getDoublesFromTable(L,4,dofs*3,currentPosVelAccel);
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,dofs*3,false,false,&errorString);
            if (res==2)
            {
                double* maxVelAccelJerk=new double[dofs*3];
                getDoublesFromTable(L,5,dofs*3,maxVelAccelJerk);
                res=checkOneGeneralInputArgument(L,6,lua_arg_bool,dofs,false,false,&errorString);
                if (res==2)
                {
                    char* selection=new char[dofs];
                    getBoolsFromTable(L,6,dofs,selection);
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,dofs*2,false,false,&errorString);
                    if (res==2)
                    {
                        double* targetPosVel=new double[dofs*2];
                        getDoublesFromTable(L,7,dofs*2,targetPosVel);

                        double* newPosVelAccel=new double[dofs*3];
                        unsigned char auxData[1+8+8];
                        auxData[0]=1;
                        retVal=simRMLPosition_internal(dofs,timeStep,flags,currentPosVelAccel,maxVelAccelJerk,(unsigned char*)selection,targetPosVel,newPosVelAccel,auxData);
                        if (retVal>=0)
                        {
                            luaWrap_lua_pushinteger(L,retVal);
                            pushDoubleTableOntoStack(L,dofs*3,newPosVelAccel);
                            luaWrap_lua_pushnumber(L,((double*)(auxData+1))[0]);
                        }
                        delete[] newPosVelAccel;
                        delete[] targetPosVel;
                    }
                    delete[] selection;
                }
                delete[] maxVelAccelJerk;
            }
            delete[] currentPosVelAccel;
            if (retVal>=0)
            {
                LUA_END(3);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRMLVelocity(luaWrap_lua_State* L)
{ // DEPRECATED since release 3.1.2
    TRACE_LUA_API;
    LUA_START("simRMLVelocity");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* scr=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (!scr->checkAndSetWarning_simRMLVelocity_oldCompatibility_30_8_2014())
        warningString="Function is deprecated. Use simRMLVel instead.";

    int retVal=-43; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int dofs=luaToInt(L,1);
        double timeStep=luaWrap_lua_tonumber(L,2);
        int flags=luaToInt(L,3);
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,dofs*3,false,false,&errorString);
        if (res==2)
        {
            double* currentPosVelAccel=new double[dofs*3];
            getDoublesFromTable(L,4,dofs*3,currentPosVelAccel);
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,dofs*2,false,false,&errorString);
            if (res==2)
            {
                double* maxAccelJerk=new double[dofs*2];
                getDoublesFromTable(L,5,dofs*2,maxAccelJerk);
                res=checkOneGeneralInputArgument(L,6,lua_arg_bool,dofs,false,false,&errorString);
                if (res==2)
                {
                    char* selection=new char[dofs];
                    getBoolsFromTable(L,6,dofs,selection);
                    res=checkOneGeneralInputArgument(L,7,lua_arg_number,dofs,false,false,&errorString);
                    if (res==2)
                    {
                        double* targetVel=new double[dofs];
                        getDoublesFromTable(L,7,dofs,targetVel);

                        double* newPosVelAccel=new double[dofs*3];
                        unsigned char auxData[1+8+8];
                        auxData[0]=1;
                        retVal=simRMLVelocity_internal(dofs,timeStep,flags,currentPosVelAccel,maxAccelJerk,(unsigned char*)selection,targetVel,newPosVelAccel,auxData);
                        if (retVal>=0)
                        {
                            luaWrap_lua_pushinteger(L,retVal);
                            pushDoubleTableOntoStack(L,dofs*3,newPosVelAccel);
                            luaWrap_lua_pushnumber(L,((double*)(auxData+1))[0]);
                        }
                        delete[] newPosVelAccel;
                        delete[] targetVel;
                    }
                    delete[] selection;
                }
                delete[] maxAccelJerk;
            }
            delete[] currentPosVelAccel;
            if (retVal>=0)
            {
                LUA_END(3);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCopyPasteSelectedObjects(luaWrap_lua_State* L)
{ // DEPRECATED since release 3.1.3
    TRACE_LUA_API;
    LUA_START("simCopyPasteSelectedObjects");

    int retVal=simCopyPasteSelectedObjects_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simFindIkPath(luaWrap_lua_State* L)
{ // DEPRECATED since 3.3.0
    TRACE_LUA_API;
    LUA_START("simFindIkPath");
    errorString="Not supported anymore.";
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetPathPlanningHandle(luaWrap_lua_State* L)
{ // DEPRECATED since 3.3.0
    TRACE_LUA_API;
    LUA_START("simGetPathPlanningHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetPathPlanningHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSearchPath(luaWrap_lua_State* L)
{ // DEPRECATED since 3.3.0
    TRACE_LUA_API;
    LUA_START("simSearchPath");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int pathPlanningObjectHandle=luaToInt(L,1);
        float maximumSearchTime=tt::getLimitedFloat(0.001f,36000.0f,luaToFloat(L,2));
        float subDt=0.05f; // 50 ms at a time (default)
        bool foundError=false;
        // Now check the optional argument:
        int res;
        if (!foundError) // sub-dt argument:
        {
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,true,&errorString);
            if (res==2)
            { // get the data
                subDt=tt::getLimitedFloat(0.001f,std::min<float>(1.0f,maximumSearchTime),luaToFloat(L,3));
            }
            foundError=(res==-1);
        }
        if (!foundError)
        {
            CPathPlanningTask* it=App::currentWorld->pathPlanning->getObject(pathPlanningObjectHandle);
            if (it==nullptr)
                errorString=SIM_ERROR_PATH_PLANNING_OBJECT_INEXISTANT;
            else
            {
                retVal=0;
                //if (VThread::isCurrentThreadTheMainSimulationThread())
                { // non-threaded
                    if (it->performSearch(false,maximumSearchTime))
                        retVal=1;
                }
                /*
                else
                { // threaded call:
                    CPathPlanningTask* oldIt=it;
                    it=oldIt->copyYourself(); // we copy it because the original might be destroyed at any time
                    it->setOriginalTask(oldIt);
                    retVal=-1; // for error
                    bool err=false;
                    if (it->initiateSteppedSearch(false,maximumSearchTime,subDt))
                    {
                        retVal=-2; // means search not yet finished
                        while (retVal==-2)
                        {
                            retVal=it->performSteppedSearch();
                            if (retVal==-2)
                            { // we are not yet finished with the search!
                                CThreadPool::switchBackToPreviousThread();
                                if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                                {
                                    errorString="@yield"; // yield will be triggered at end of this function
                                    retVal=-1; // generate an error
                                    break; // will generate an error (retVal is -1)
                                }
                            }
                        }
                        if (retVal==-1)
                        {
                            errorString=SIM_ERROR_PATH_PLANNING_OBJECT_NOT_CONSISTENT_ANYMORE;
                            retVal=-1; // for error
                            err=true;
                        }
                        // Return values are -1 (error), 0 (no path found) 1 (partial path found) and 2 (full path found)
                    }
                    else
                    { // the task is not consistent!
                        errorString=SIM_ERROR_PATH_PLANNING_OBJECT_NOT_CONSISTENT;
                        err=true;
                    }

                    if (!err)
                    {
                        CPathPlanningTask* originalIt=it->getOriginalTask();
                        int tree1Handle,tree2Handle;
                        it->getAndDisconnectSearchTrees(tree1Handle,tree2Handle); // to keep trees visible!
                        delete it;
                        // Now we connect the trees only if the originalTask still exists:
                        bool found=false;
                        for (int ot=0;ot<int(App::currentWorld->pathPlanning->allObjects.size());ot++)
                        {
                            if (App::currentWorld->pathPlanning->allObjects[ot]==originalIt)
                            {
                                found=true;
                                break;
                            }
                        }
                        if (found)
                            originalIt->connectExternalSearchTrees(tree1Handle,tree2Handle);
                        else
                        {
                            App::currentWorld->drawingCont->removeObject(tree1Handle);
                            App::currentWorld->drawingCont->removeObject(tree2Handle);
                        }
                    }
                }
                */
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simInitializePathSearch(luaWrap_lua_State* L)
{ // DEPRECATED since 3.3.0
    TRACE_LUA_API;
    LUA_START("simInitializePathSearch");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int pathPlanningObjectHandle=luaToInt(L,1);
        float maximumSearchTime=luaToFloat(L,2);
        float searchTimeStep=luaToFloat(L,3);
        retVal=simInitializePathSearch_internal(pathPlanningObjectHandle,maximumSearchTime,searchTimeStep);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simPerformPathSearchStep(luaWrap_lua_State* L)
{ // DEPRECATED since 3.3.0
    TRACE_LUA_API;
    LUA_START("simPerformPathSearchStep");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0))
    {
        int temporaryPathSearchObjectHandle=luaToInt(L,1);
        bool abortSearch=luaToBool(L,2);
        retVal=simPerformPathSearchStep_internal(temporaryPathSearchObjectHandle,abortSearch);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simEnableWorkThreads(luaWrap_lua_State* L)
{ // DEPRECATED since 3/4/2016
    TRACE_LUA_API;
    LUA_START("simEnableWorkThreads");
    luaWrap_lua_pushinteger(L,0);
    LUA_END(1);
}

int _simWaitForWorkThreads(luaWrap_lua_State* L)
{ // DEPRECATED since 3/4/2016
    TRACE_LUA_API;
    LUA_START("simWaitForWorkThreads");
    LUA_END(0);
}

int _simGetInvertedMatrix(luaWrap_lua_State* L)
{ // DEPRECATED since 10/5/2016
    TRACE_LUA_API;
    LUA_START("simGetInvertedMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,12))
    {
        float arr[12];
        getFloatsFromTable(L,1,12,arr);
        simInvertMatrix_internal(arr);
        pushFloatTableOntoStack(L,12,arr);
        LUA_END(1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simAddSceneCustomData(luaWrap_lua_State* L)
{ // DEPRECATED since 26/12/2016
    TRACE_LUA_API;
    LUA_START("simAddSceneCustomData");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int headerNumber=luaToInt(L,1);
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
        retVal=simAddSceneCustomData_internal(headerNumber,data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetSceneCustomData(luaWrap_lua_State* L)
{ // DEPRECATED since 26/12/2016
    TRACE_LUA_API;
    LUA_START("simGetSceneCustomData");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int headerNumber=luaToInt(L,1);
        int dataLength=simGetSceneCustomDataLength_internal(headerNumber);
        if (dataLength>=0)
        {
            char* data=new char[dataLength];
            int retVal=simGetSceneCustomData_internal(headerNumber,data);
            if (retVal==-1)
                delete[] data;
            else
            {
                luaWrap_lua_pushlstring(L,data,dataLength);
                delete[] data;
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simAddObjectCustomData(luaWrap_lua_State* L)
{ // DEPRECATED since 26/12/2016
    TRACE_LUA_API;
    LUA_START("simAddObjectCustomData");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0))
    {
        int objectHandle=luaToInt(L,1);
        int headerNumber=luaToInt(L,2);
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,3,&dataLength);
        retVal=simAddObjectCustomData_internal(objectHandle,headerNumber,data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}


int _simGetObjectCustomData(luaWrap_lua_State* L)
{ // DEPRECATED since 26/12/2016
    TRACE_LUA_API;
    LUA_START("simGetObjectCustomData");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int objectHandle=luaToInt(L,1);
        int headerNumber=luaToInt(L,2);
        int dataLength=simGetObjectCustomDataLength_internal(objectHandle,headerNumber);
        if (dataLength>=0)
        {
            char* data=new char[dataLength];
            int retVal=simGetObjectCustomData_internal(objectHandle,headerNumber,data);
            if (retVal==-1)
                delete[] data;
            else
            {
                luaWrap_lua_pushlstring(L,data,dataLength);
                delete[] data;
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetUIHandle(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetUIHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUIProperty(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIProperty");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simGetUIProperty_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUIEventButton(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIEventButton");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int auxVals[2];
        retVal=simGetUIEventButton_internal(luaToInt(L,1),auxVals);
        luaWrap_lua_pushinteger(L,retVal);
        if (retVal==-1)
        {
            LUA_END(1);
        }
        pushIntTableOntoStack(L,2,auxVals);
        LUA_END(2);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUIProperty(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIProperty");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetUIProperty_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUIButtonSize(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIButtonSize");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int size[2];
        if (simGetUIButtonSize_internal(luaToInt(L,1),luaToInt(L,2),size)==1)
        {
            pushIntTableOntoStack(L,2,size);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetUIButtonProperty(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIButtonProperty");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simGetUIButtonProperty_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUIButtonProperty(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIButtonProperty");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetUIButtonProperty_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUISlider(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUISlider");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simGetUISlider_internal(luaToInt(L,1),luaToInt(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUISlider(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUISlider");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetUISlider_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUIButtonLabel(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIButtonLabel");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        std::string stra;
        std::string strb;
        char* str1=nullptr;
        char* str2=nullptr;
        int res=checkOneGeneralInputArgument(L,3,lua_arg_string,0,false,true,&errorString);
        if (res!=-1)
        {
            if (res==2)
            {
                stra=luaWrap_lua_tostring(L,3);
                str1=(char*)stra.c_str();
            }
            res=checkOneGeneralInputArgument(L,4,lua_arg_string,0,true,true,&errorString);
            if (res!=-1)
            {
                if (res==2)
                {
                    strb=luaWrap_lua_tostring(L,4);
                    str2=(char*)strb.c_str();
                }
                retVal=simSetUIButtonLabel_internal(luaToInt(L,1),luaToInt(L,2),str1,str2);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUIButtonLabel(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIButtonLabel");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        char* label=simGetUIButtonLabel_internal(luaToInt(L,1),luaToInt(L,2));
        if (label!=nullptr)
        {
            luaWrap_lua_pushstring(L,label);
            simReleaseBuffer_internal(label);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCreateUIButtonArray(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simCreateUIButtonArray");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simCreateUIButtonArray_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUIButtonArrayColor(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIButtonArrayColor");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,2,lua_arg_number,3))
    {
        int pos[2];
        float col[3];
        getIntsFromTable(L,3,2,pos);
        getFloatsFromTable(L,4,3,col);
        retVal=simSetUIButtonArrayColor_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2),pos,col);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simDeleteUIButtonArray(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simDeleteUIButtonArray");
    int retVal=-1;// error

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simDeleteUIButtonArray_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateUI(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simCreateUI");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_string,0,lua_arg_number,0,lua_arg_number,2,lua_arg_number,2))
    {
        int clientSize[2];
        int cellSize[2];
        getIntsFromTable(L,3,2,clientSize);
        getIntsFromTable(L,4,2,cellSize);
        int menuAttributes=luaWrap_lua_tointeger(L,2);
        int b=0;
        for (int i=0;i<8;i++)
        {
            if (menuAttributes&(1<<i))
                b++;
        }
        int* buttonHandles=new int[b];
        menuAttributes=(menuAttributes|sim_ui_menu_systemblock)-sim_ui_menu_systemblock;
        retVal=simCreateUI_internal(luaWrap_lua_tostring(L,1),menuAttributes,clientSize,cellSize,buttonHandles);
        if (retVal!=-1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushIntTableOntoStack(L,b,buttonHandles);
            delete[] buttonHandles;
            LUA_END(2);
        }
        delete[] buttonHandles;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateUIButton(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simCreateUIButton");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,2,lua_arg_number,2,lua_arg_number,0))
    {
        int pos[2];
        int size[2];
        getIntsFromTable(L,2,2,pos);
        getIntsFromTable(L,3,2,size);
        retVal=simCreateUIButton_internal(luaWrap_lua_tointeger(L,1),pos,size,luaWrap_lua_tointeger(L,4));
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSaveUI(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSaveUI");

    int retVal=-1;// error
    int res=checkOneGeneralInputArgument(L,1,lua_arg_table,1,false,true,&errorString);
    if (res>0)
    {
        int res2=checkOneGeneralInputArgument(L,2,lua_arg_string,0,false,false,&errorString);
        if (res2==2)
        {
            if (res==1)
                retVal=simSaveUI_internal(0,nullptr,luaWrap_lua_tostring(L,2));
            else
            {
                int tl=int(luaWrap_lua_rawlen(L,1));
                int* tble=new int[tl];
                getIntsFromTable(L,1,tl,tble);
                retVal=simSaveUI_internal(tl,tble,luaWrap_lua_tostring(L,2));
                delete[] tble;
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLoadUI(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simLoadUI");
    LUA_END(0);
}

int _simRemoveUI(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simRemoveUI");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simRemoveUI_internal(luaWrap_lua_tointeger(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUIPosition(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simGetUIPosition");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int position[2];
        if (simGetUIPosition_internal(luaToInt(L,1),position)!=-1)
        {
            pushIntTableOntoStack(L,2,position);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetUIPosition(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIPosition");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,2))
    {
        int position[2];
        getIntsFromTable(L,2,2,position);
        retVal=simSetUIPosition_internal(luaToInt(L,1),position);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetUIButtonColor(luaWrap_lua_State* L)
{ // DEPRECATED since 09/02/2017
    TRACE_LUA_API;
    LUA_START("simSetUIButtonColor");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float col[9];
        float* acp[3]={col,col+3,col+6};
//      bool failed=true;
        int res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,false,true,&errorString);
        if (res!=-1)
        {
            if (res!=2)
                acp[0]=nullptr;
            res=checkOneGeneralInputArgument(L,4,lua_arg_number,3,true,true,&errorString);
            if (res!=-1)
            {
                if (res!=2)
                    acp[1]=nullptr;
                res=checkOneGeneralInputArgument(L,5,lua_arg_number,3,true,true,&errorString);
                if (res!=-1)
                {
                    if (res!=2)
                        acp[2]=nullptr;
                    for (int i=0;i<3;i++)
                    {
                        if (acp[i]!=nullptr)
                            getFloatsFromTable(L,3+i,3,acp[i]);
                    }
                    retVal=simSetUIButtonColor_internal(luaToInt(L,1),luaToInt(L,2),acp[0],acp[1],acp[2]);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleChildScript(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simHandleChildScript");

    warningString="function is deprecated. Use simHandleChildScripts instead.";
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (!it->checkAndSetWarningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014())
    {
        std::string title("Compatibility issue with ");
        title+=it->getShortDescriptiveName();
        std::string txt("The command simHandleChildScript is not supported anymore and was replaced&&n");
        txt+="with sim.handleChildScripts, which operates in a slightly different manner. Make sure to&&n";
        txt+="adjust this script manually.";
        simDisplayDialog_internal(title.c_str(),txt.c_str(),sim_dlgstyle_ok,"",nullptr,nullptr,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleSensingChildScripts(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simHandleSensingChildScripts");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simBoolOr16(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simBoolOr16");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<65535.9)&&(nb<65535.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            retVal=a|b;
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simBoolAnd16(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simBoolAnd16");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<65535.9)&&(nb<65535.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            retVal=a&b;
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simBoolXor16(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simBoolXor16");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        luaWrap_lua_Number na=luaWrap_lua_tonumber(L,1);
        luaWrap_lua_Number nb=luaWrap_lua_tonumber(L,2);
        if ((na>=0)&&(nb>=0)&&(na<65535.9)&&(nb<65535.9))
        {
            unsigned int a=(unsigned int)na;
            unsigned int b=(unsigned int)nb;
            retVal=a^b;
        }
        else
            errorString=SIM_ERROR_INVALID_NUMBER_INPUT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simScaleSelectedObjects(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simScaleSelectedObjects");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_bool,0))
        retVal=simScaleSelectedObjects_internal(luaToFloat(L,1),luaToBool(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simDeleteSelectedObjects(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simDeleteSelectedObjects");

    int retVal=simDeleteSelectedObjects_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simDelegateChildScriptExecution(luaWrap_lua_State* L)
{ // DEPRECATED. can only be called from a script running in a thread!!
    TRACE_LUA_API;
    LUA_START("simDelegateChildScriptExecution");

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetShapeVertex(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetShapeVertex");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        float relPos[3];
        retVal=simGetShapeVertex_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),relPos);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushFloatTableOntoStack(L,3,relPos);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeTriangle(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetShapeTriangle");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int indices[3];
        float normals[9];
        retVal=simGetShapeTriangle_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),indices,normals);
        if (retVal==1)
        {
            luaWrap_lua_pushinteger(L,retVal);
            pushIntTableOntoStack(L,3,indices);
            pushFloatTableOntoStack(L,9,normals);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetMaterialId(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetMaterialId");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string matName(luaWrap_lua_tostring(L,1));
        retVal=simGetMaterialId_internal(matName.c_str());
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetShapeMaterial(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetShapeMaterial");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int shapeHandle=luaToInt(L,1);
        retVal=simGetShapeMaterial_internal(shapeHandle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReleaseScriptRawBuffer(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simReleaseScriptRawBuffer");

    int retVal=-1; //error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
            handle=getCurrentScriptHandle(L);
        if ( (handle!=sim_handle_tree)&&(handle!=sim_handle_chain) )
            retVal=simReleaseScriptRawBuffer_internal(handle,luaToInt(L,2));
        else
        {
            std::vector<int> scriptHandles;
            if (handle==sim_handle_tree)
                getScriptTree(L,false,scriptHandles);
            else
                getScriptChain(L,false,false,scriptHandles);
            for (int i=0;i<int(scriptHandles.size());i++)
                retVal=simReleaseScriptRawBuffer_internal(scriptHandles[i],sim_handle_all);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectSelectionSize(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetObjectSelectionSize");

    int retVal=simGetObjectSelectionSize_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetObjectLastSelection(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("simGetObjectLastSelection");

    int retVal=simGetObjectLastSelection_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetVisionSensorFilter(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.setVisionSensorFilter");
    int retVal=-1; // error

    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetVisionSensorFilter(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.getVisionSensorFilter");

    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}

int _simGetScriptSimulationParameter(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.getScriptSimulationParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        bool goOn=true;
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
        {
            handle=getCurrentScriptHandle(L);
            // Since this routine can also be called by customization scripts, check for that here:
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(handle);
            if (it->getScriptType()==sim_scripttype_customizationscript)
            {
                handle=it->getObjectIDThatScriptIsAttachedTo_customization();
                it=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(handle);
                if (it!=nullptr)
                    handle=it->getScriptID();
                else
                    goOn=false;
            }
        }
        if (goOn)
        {
            bool returnString=false;
            int ret=checkOneGeneralInputArgument(L,3,lua_arg_bool,0,true,false,&errorString);
            if ((ret==0)||(ret==2))
            {
                if (ret==2)
                    returnString=luaToBool(L,3);
                std::string parameterName(luaWrap_lua_tostring(L,2));
                if ( (handle!=sim_handle_tree)&&(handle!=sim_handle_chain)&&(handle!=sim_handle_all) )
                {
                    int l;
                    char* p=simGetScriptSimulationParameter_internal(handle,parameterName.c_str(),&l);
                    if (p!=nullptr)
                    {
                        std::string a;
                        a.assign(p,l);
                        if (returnString)
                            luaWrap_lua_pushlstring(L,a.c_str(),a.length());
                        else
                            pushCorrectTypeOntoLuaStack(L,a);
                        simReleaseBuffer_internal(p);
                        LUA_END(1);
                    }
                }
                else
                {
                    std::vector<int> scriptHandles;
                    if (handle==sim_handle_tree)
                        getScriptTree(L,false,scriptHandles);
                    if (handle==sim_handle_chain)
                        getScriptChain(L,false,false,scriptHandles);
                    if (handle==sim_handle_all)
                    {
                        for (int i=0;i<int(App::currentWorld->luaScriptContainer->allScripts.size());i++)
                        {
                            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->allScripts[i];
                            int scrType=it->getScriptType();
                            if ((scrType==sim_scripttype_mainscript)||(scrType==sim_scripttype_childscript)) // make sure plugin script etc. are not included!
                                scriptHandles.push_back(it->getScriptID());
                        }
                    }
                    std::vector<std::string> retParams;
                    std::vector<int> retHandles;
                    for (int i=0;i<int(scriptHandles.size());i++)
                    {
                        int l;
                        char* p=simGetScriptSimulationParameter_internal(scriptHandles[i],parameterName.c_str(),&l);
                        if (p!=nullptr)
                        {
                            std::string a;
                            a.assign(p,l);
                            simReleaseBuffer_internal(p);
                            retParams.push_back(a);
                            retHandles.push_back(scriptHandles[i]);
                        }
                    }
                    if (retParams.size()!=0)
                    { // now we push two tables onto the stack:
                        CInterfaceStack stack;
                        stack.pushTableOntoStack();
                        for (int i=0;i<int(retParams.size());i++)
                        {
                            stack.pushNumberOntoStack((double)i+1); // key
                            int t=getCorrectType(retParams[i]);
                            if (returnString)
                                t=4; // we force for strings!
                            if (t==0)
                                stack.pushNullOntoStack();
                            if ((t==1)||(t==2))
                                stack.pushBoolOntoStack(t==2);
                            if (t==3)
                            {
                                float v;
                                tt::getValidFloat(retParams[i].c_str(),v);
                                stack.pushNumberOntoStack((double)v);
                            }
                            if (t==4)
                                stack.pushStringOntoStack(retParams[i].c_str(),0);
                            if (stack.getStackSize()<2)
                                stack.pushNullOntoStack();
                            stack.insertDataIntoStackTable();
                        }
                        stack.buildOntoLuaStack(L,true);
                        pushIntTableOntoStack(L,(int)retHandles.size(),&retHandles[0]);
                        LUA_END(2);
                    }
                }
            }
        }
        else
            errorString=SIM_ERROR_NO_ASSOCIATED_CHILD_SCRIPT_FOUND;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetScriptSimulationParameter(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.setScriptSimulationParameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_string,0))
    {
        bool goOn=true;
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
        {
            handle=getCurrentScriptHandle(L);
            // Since this routine can also be called by customization scripts, check for that here:
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(handle);
            if (it->getScriptType()==sim_scripttype_customizationscript)
            {
                handle=it->getObjectIDThatScriptIsAttachedTo_customization();
                it=App::currentWorld->luaScriptContainer->getScriptFromObjectAttachedTo_child(handle);
                if (it!=nullptr)
                    handle=it->getScriptID();
                else
                    goOn=false;
            }
        }
        if (goOn)
        {
            std::string parameterName(luaWrap_lua_tostring(L,2));
            size_t parameterValueLength;
            char* parameterValue=(char*)luaWrap_lua_tolstring(L,3,&parameterValueLength);
            if ( (handle!=sim_handle_tree)&&(handle!=sim_handle_chain) )
            {
                retVal=simSetScriptSimulationParameter_internal(handle,parameterName.c_str(),parameterValue,(int)parameterValueLength);
            }
            else
            {
                std::vector<int> scriptHandles;
                if (handle==sim_handle_tree)
                    getScriptTree(L,false,scriptHandles);
                else
                    getScriptChain(L,false,false,scriptHandles);
                retVal=0;
                for (size_t i=0;i<scriptHandles.size();i++)
                {
                    if (simSetScriptSimulationParameter_internal(scriptHandles[i],parameterName.c_str(),parameterValue,(int)parameterValueLength)==1)
                        retVal++;
                }
            }
        }
        else
            errorString=SIM_ERROR_NO_ASSOCIATED_CHILD_SCRIPT_FOUND;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simHandleMechanism(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.handleMechanism");
    int retVal=-1; // means error
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetPathTargetNominalVelocity(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.setPathTargetNominalVelocity");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetPathTargetNominalVelocity_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetNameSuffix(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.getNameSuffix");

    if (checkInputArguments(L,nullptr,lua_arg_nil,0))
    { // we want the suffix of current script
        luaWrap_lua_pushinteger(L,getScriptNameIndex(L));
        LUA_END(1);
    }
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    { // we want the suffix of the provided name
        std::string nameWithSuffix(luaWrap_lua_tostring(L,1));
        std::string name(tt::getNameWithoutSuffixNumber(nameWithSuffix.c_str(),true));
        int suffixNumber=tt::getNameSuffixNumber(nameWithSuffix.c_str(),true);
        luaWrap_lua_pushinteger(L,suffixNumber);
        luaWrap_lua_pushstring(L,name.c_str());
        LUA_END(2);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetNameSuffix(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.setNameSuffix");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int nb=luaWrap_lua_tointeger(L,1);
        setScriptNameIndex(L,nb);
        retVal=1;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simAddStatusbarMessage(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.addStatusbarMessage");

    int retVal=-1; // means error
    if (luaWrap_lua_gettop(L)==0)
    {
        App::clearStatusbar();
        retVal=1;
    }
    else
    {
        if (checkInputArguments(L,nullptr,lua_arg_nil,0))
        {
            App::clearStatusbar();
            retVal=1;
        }
        else
        {
            if (checkInputArguments(L,&errorString,lua_arg_string,0))
            {
                //retVal=simAddStatusbarMessage_internal(luaWrap_lua_tostring(L,1));
                CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(getCurrentScriptHandle(L));
                if (it!=nullptr)
                {
                    App::logScriptMsg(it->getShortDescriptiveName().c_str(),sim_verbosity_msgs,luaWrap_lua_tostring(L,1));
                    retVal=1;
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetShapeMassAndInertia(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.setShapeMassAndInertia");

    int result=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,9,lua_arg_number,3))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float mass=luaToFloat(L,2);
        float inertiaMatrix[9];
        getFloatsFromTable(L,3,9,inertiaMatrix);
        float centerOfMass[3];
        getFloatsFromTable(L,4,3,centerOfMass);
        float* transf=nullptr;
        float transformation[12];
        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,12,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,5,12,transformation);
                transf=transformation;
            }
            result=simSetShapeMassAndInertia_internal(handle,mass,inertiaMatrix,centerOfMass,transf);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,result);
    LUA_END(1);
}

int _simGetShapeMassAndInertia(luaWrap_lua_State* L)
{ // DEPRECATED
    TRACE_LUA_API;
    LUA_START("sim.getShapeMassAndInertia");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        float* transf=nullptr;
        float transformation[12];
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,12,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getFloatsFromTable(L,2,12,transformation);
                transf=transformation;
            }
            float mass;
            float inertiaMatrix[9];
            float centerOfMass[3];
            int result=simGetShapeMassAndInertia_internal(handle,&mass,inertiaMatrix,centerOfMass,transf);
            if (result==-1)
                luaWrap_lua_pushnil(L);
            luaWrap_lua_pushnumber(L,mass);
            pushFloatTableOntoStack(L,9,inertiaMatrix);
            pushFloatTableOntoStack(L,3,centerOfMass);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}


int _simMoveToObject(luaWrap_lua_State* L)
{ // Deprecated on 08.09.2020
    TRACE_LUA_API;
    LUA_START("sim.moveToObject");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (!(CThreadPool::getSimulationStopRequested()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L))))
        { // Important to first check if we are supposed to leave the thread!!
            if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
            { // Those are the arguments that are always required! (the rest can be ignored or set to nil!
                int objID=luaWrap_lua_tointeger(L,1);
                int targetObjID=luaWrap_lua_tointeger(L,2);
                float maxVelocity=0.1f;
                float relativeDistanceOnPath=-1.0f;
                int positionAndOrOrientation=3; // position and orientation (default value)
                CSceneObject* object=App::currentWorld->sceneObjects->getObjectFromHandle(objID);
                CSceneObject* targetObject=App::currentWorld->sceneObjects->getObjectFromHandle(targetObjID);
                float accel=0.0f; // means infinite accel!! (default value)
                bool foundError=false;
                if ((!foundError)&&((object==nullptr)||(targetObject==nullptr)))
                {
                    errorString=SIM_ERROR_OBJECT_OR_TARGET_OBJECT_DOES_NOT_EXIST;
                    foundError=true;
                }
                if ((!foundError)&&(targetObject==object))
                {
                    errorString=SIM_ERROR_OBJECT_IS_SAME_AS_TARGET_OBJECT;
                    foundError=true;
                }
                // Now check the optional arguments:
                int res;
                if (!foundError) // position and/or orientation argument:
                {
                    res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,true,&errorString);
                    if (res==2)
                    { // get the data
                        positionAndOrOrientation=abs(luaToInt(L,3));
                        if ((positionAndOrOrientation&3)==0)
                            positionAndOrOrientation=1; // position only
                    }
                    foundError=(res==-1);
                }
                if (!foundError) // positionOnPath argument:
                {
                    res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,true,&errorString);
                    if (res==2)
                    { // get the data
                        relativeDistanceOnPath=tt::getLimitedFloat(0.0f,1.0f,luaToFloat(L,4));
                        if (targetObject->getObjectType()!=sim_object_path_type)
                        {
                            errorString=SIM_ERROR_TARGET_OBJECT_IS_NOT_A_PATH;
                            foundError=true;
                        }
                    }
                    foundError=(res==-1);
                }
                if (!foundError) // Velocity argument:
                {
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,false,false,&errorString);
                    if (res==2)
                    { // get the data
                        maxVelocity=luaToFloat(L,5);
                    }
                    else
                        foundError=true; // this argument is not optional!
                }
                if (!foundError) // Accel argument:
                {
                    res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,true,&errorString);
                    if (res==2)
                    { // get the data
                        accel=fabs(luaToFloat(L,6));
                    }
                    foundError=(res==-1);
                }
                if (!foundError)
                { // do the job here!
                    C7Vector startTr(object->getCumulativeTransformation());
                    float currentVel=0.0f;
                    CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                    float lastTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+threadData->usedMovementTime;
                    float vdl=1.0f;
                    // vld is the totalvirtual distance
                    float currentPos=0.0f;

                    bool movementFinished=false;
                    float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f; // this is the time left if we leave here
                    float previousLL=0.0f;
                    bool err=false;
                    while ( (!movementFinished)&&(vdl!=0.0f) )
                    {
                        float currentTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
                        dt=currentTime-lastTime;
                        lastTime=currentTime;

                        if (accel==0.0f)
                        { // Means infinite acceleration
                            float timeNeeded=(vdl-currentPos)/maxVelocity;
                            currentVel=maxVelocity;
                            if (timeNeeded>dt)
                            {
                                currentPos+=dt*maxVelocity;
                                dt=0.0f; // this is what is left
                            }
                            else
                            {
                                currentPos=vdl;
                                if (timeNeeded>=0.0f)
                                    dt-=timeNeeded;
                            }
                        }
                        else
                        {
                            double p=currentPos;
                            double v=currentVel;
                            double t=dt;
                            CLinMotionRoutines::getNextValues(p,v,maxVelocity,accel,0.0f,vdl,0.0f,0.0f,t);
                            currentPos=float(p);
                            currentVel=float(v);
                            dt=float(t);
                        }

                        // Now check if we are within tolerances:
                        if (fabs(currentPos-vdl)<=0.00001f)
                            movementFinished=true;

                        // Set the new configuration of the object:
                        float ll=currentPos/vdl;
                        if (ll>1.0f)
                            ll=1.0f;
                        if ((App::currentWorld->sceneObjects->getObjectFromHandle(objID)==object)&&(App::currentWorld->sceneObjects->getObjectFromHandle(targetObjID)==targetObject)) // make sure the objects are still valid (running in a thread)
                        {
                            C7Vector targetTr(targetObject->getCumulativeTransformation());
                            bool goOn=true;
                            if (relativeDistanceOnPath>=0.0f)
                            { // we should have a path here
                                if (targetObject->getObjectType()==sim_object_path_type)
                                {
                                    C7Vector pathLoc;
                                    if ( ((CPath*)targetObject)->pathContainer->getTransformationOnBezierCurveAtNormalizedVirtualDistance(relativeDistanceOnPath,pathLoc))
                                        targetTr*=pathLoc;
                                    else
                                        relativeDistanceOnPath=-1.0f; // the path is empty!
                                }
                                else
                                    goOn=false;
                            }
                            if (goOn)
                            {
                                C7Vector newAbs;
                                newAbs.buildInterpolation(startTr,targetTr,(ll-previousLL)/(1.0f-previousLL));
                                startTr=newAbs;
                                C7Vector parentInv(object->getFullParentCumulativeTransformation().getInverse());
                                C7Vector currentTr(object->getCumulativeTransformation());
                                if ((positionAndOrOrientation&1)==0)
                                    newAbs.X=currentTr.X;
                                if ((positionAndOrOrientation&2)==0)
                                    newAbs.Q=currentTr.Q;
                                object->setLocalTransformation(parentInv*newAbs);
                            }
                            else
                                movementFinished=true; // the target object is not a path anymore!!
                        }
                        else
                            movementFinished=true; // the object was destroyed during execution of the command!
                        previousLL=ll;
                        if (!movementFinished)
                        {
                            CThreadPool::switchBackToPreviousThread();
                            if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                            {
                                errorString="@yield"; // yield will be triggered at end of this function
                                err=true;
                                break;
                            }
                        }
                    }
                    if (!err)
                    {
                        // The movement finished. Now add the time used:
                        threadData->usedMovementTime=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f-dt;
                        luaWrap_lua_pushnumber(L,dt); // success (deltaTime left)
                        LUA_END(1);
                    }
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simCheckIkGroup(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.checkIkGroup");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        if (luaWrap_lua_istable(L,2))
        {
            int jointCnt=(int)luaWrap_lua_rawlen(L,2);
            int* handles=new int[jointCnt];
            getIntsFromTable(L,2,jointCnt,handles);
            float* values=new float[jointCnt];

            int res=checkOneGeneralInputArgument(L,3,lua_arg_number,jointCnt,true,true,&errorString);
            if (res>=0)
            {
                int* jointOptionsP=nullptr;
                std::vector<int> jointOptions;
                if (res==2)
                {
                    jointOptions.resize(jointCnt);
                    getIntsFromTable(L,3,jointCnt,&jointOptions[0]);
                    jointOptionsP=&jointOptions[0];
                }

                int retVal=simCheckIkGroup_internal(luaToInt(L,1),jointCnt,handles,values,jointOptionsP);
                luaWrap_lua_pushinteger(L,retVal);
                pushFloatTableOntoStack(L,jointCnt,values);
                delete[] values;
                delete[] handles;
                LUA_END(2);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,-1);
    LUA_END(1);
}

int _simCreateIkGroup(luaWrap_lua_State* L)
{ // DEPRECATED ON 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.createIkGroup");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int options=luaToInt(L,1);
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            int intParams[2];
            int* intP=nullptr;
            if (res==2)
            {
                getIntsFromTable(L,2,2,intParams);
                intP=intParams;
            }
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,4,true,true,&errorString);
            if (res>=0)
            {
                float floatParams[4];
                float* floatP=nullptr;
                if (res==2)
                {
                    getFloatsFromTable(L,3,4,floatParams);
                    floatP=floatParams;
                }
                retVal=simCreateIkGroup_internal(options,intP,floatP,nullptr);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simRemoveIkGroup(luaWrap_lua_State* L)
{ // DEPRECATED ON 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.removeIkGroup");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int handle=luaToInt(L,1);
        retVal=simRemoveIkGroup_internal(handle);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreateIkElement(luaWrap_lua_State* L)
{ // DEPRECATED ON 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.createIkElement");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,4))
    {
        int ikGroup=luaToInt(L,1);
        int options=luaToInt(L,2);
        int intParams[4];
        getIntsFromTable(L,3,4,intParams);

        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,4,true,true,&errorString);
        if (res>=0)
        {
            float floatParams[4];
            float* floatP=nullptr;
            if (res==2)
            {
                getFloatsFromTable(L,4,4,floatParams);
                floatP=floatParams;
            }
            retVal=simCreateIkElement_internal(ikGroup,options,intParams,floatP,nullptr);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simExportIk(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.exportIk");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string pathAndFilename(luaWrap_lua_tostring(L,1));
        retVal=simExportIk_internal(pathAndFilename.c_str(),0,nullptr);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simComputeJacobian(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.computeJacobian");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simComputeJacobian_internal(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2),nullptr);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetConfigForTipPose(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.getConfigForTipPose");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,1,lua_arg_number,0,lua_arg_number,0))
    {
        int ikGroupHandle=luaWrap_lua_tointeger(L,1);
        std::vector<int> jointHandles;
        int jointCnt=int(luaWrap_lua_rawlen(L,2));
        jointHandles.resize(jointCnt);
        getIntsFromTable(L,2,jointCnt,&jointHandles[0]);
        float thresholdDist=luaWrap_lua_tonumber(L,3);
        int maxTimeInMs=luaWrap_lua_tointeger(L,4);
        float metric[4]={1.0,1.0,1.0,0.1f};
        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,4,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
                getFloatsFromTable(L,5,4,metric);
            int collisionPairCnt=0;
            std::vector<int> _collisionPairs;
            int* collisionPairs=nullptr;
            res=checkOneGeneralInputArgument(L,6,lua_arg_number,-1,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    collisionPairCnt=int(luaWrap_lua_rawlen(L,6))/2;
                    if (collisionPairCnt>0)
                    {
                        _collisionPairs.resize(collisionPairCnt*2);
                        getIntsFromTable(L,6,collisionPairCnt*2,&_collisionPairs[0]);
                        collisionPairs=&_collisionPairs[0];
                    }
                }
                std::vector<int> _jointOptions;
                int* jointOptions=nullptr;
                res=checkOneGeneralInputArgument(L,7,lua_arg_number,jointCnt,true,true,&errorString);
                if (res>=0)
                {
                    if (res==2)
                    {
                        _jointOptions.resize(jointCnt);
                        getIntsFromTable(L,7,jointCnt,&_jointOptions[0]);
                        jointOptions=&_jointOptions[0];
                    }

                    res=checkOneGeneralInputArgument(L,8,lua_arg_number,jointCnt,true,true,&errorString);
                    if (res>=0)
                    {
                        std::vector<float> _lowLimits;
                        _lowLimits.resize(jointCnt);
                        std::vector<float> _ranges;
                        _ranges.resize(jointCnt);
                        float* lowLimits=nullptr;
                        float* ranges=nullptr;
                        if (res==2)
                        {
                            getFloatsFromTable(L,8,jointCnt,&_lowLimits[0]);
                            lowLimits=&_lowLimits[0];
                        }
                        res=checkOneGeneralInputArgument(L,9,lua_arg_number,jointCnt,lowLimits==nullptr,lowLimits==nullptr,&errorString);
                        if (res>=0)
                        {
                            if (res==2)
                            {
                                getFloatsFromTable(L,9,jointCnt,&_ranges[0]);
                                ranges=&_ranges[0];
                            }
                            std::vector<float> foundConfig;
                            foundConfig.resize(jointCnt);
                            res=simGetConfigForTipPose_internal(ikGroupHandle,jointCnt,&jointHandles[0],thresholdDist,maxTimeInMs,&foundConfig[0],metric,collisionPairCnt,collisionPairs,jointOptions,lowLimits,ranges,nullptr);
                            if (res>0)
                            {
                                pushFloatTableOntoStack(L,jointCnt,&foundConfig[0]);
                                LUA_END(1);
                            }
                        }
                    }
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGenerateIkPath(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.generateIkPath");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,1,lua_arg_number,0))
    {
        int ikGroupHandle=luaWrap_lua_tointeger(L,1);
        std::vector<int> jointHandles;
        int jointCnt=int(luaWrap_lua_rawlen(L,2));
        jointHandles.resize(jointCnt);
        getIntsFromTable(L,2,jointCnt,&jointHandles[0]);
        int ptCnt=luaWrap_lua_tonumber(L,3);
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,-1,true,true,&errorString);
        if (res>=0)
        {
            int collisionPairCnt=0;
            std::vector<int> _collisionPairs;
            int* collisionPairs=nullptr;
            if (res==2)
            {
                collisionPairCnt=int(luaWrap_lua_rawlen(L,4))/2;
                if (collisionPairCnt>0)
                {
                    _collisionPairs.resize(collisionPairCnt*2);
                    getIntsFromTable(L,4,collisionPairCnt*2,&_collisionPairs[0]);
                    collisionPairs=&_collisionPairs[0];
                }
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,jointCnt,true,true,&errorString);
            if (res>=0)
            {
                std::vector<int> _jointOptions;
                int* jointOptions=nullptr;
                if (res==2)
                {
                    _jointOptions.resize(jointCnt);
                    getIntsFromTable(L,5,jointCnt,&_jointOptions[0]);
                    jointOptions=&_jointOptions[0];
                }
                float* path=simGenerateIkPath_internal(ikGroupHandle,jointCnt,&jointHandles[0],ptCnt,collisionPairCnt,collisionPairs,jointOptions,nullptr);
                if (path!=nullptr)
                {
                    pushFloatTableOntoStack(L,jointCnt*ptCnt,path);
                    simReleaseBuffer_internal((char*)path);
                    LUA_END(1);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetIkGroupHandle(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.getIkGroupHandle");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_string,0))
    {
        std::string name(luaWrap_lua_tostring(L,1));
        setCurrentScriptNameIndex_cSide(getScriptNameIndex(L)); // important for automatic name adjustment
        retVal=simGetIkGroupHandle_internal(name.c_str());
        setCurrentScriptNameIndex_cSide(-1);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetIkGroupMatrix(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.getIkGroupMatrix");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int ikGroupHandle=luaToInt(L,1);
        int options=luaToInt(L,2);
        int matrixSize[2];
        float* data=simGetIkGroupMatrix_internal(ikGroupHandle,options,matrixSize);
        if (data!=nullptr)
        {
            pushFloatTableOntoStack(L,matrixSize[0]*matrixSize[1],data);
            pushIntTableOntoStack(L,2,matrixSize);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simHandleIkGroup(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.handleIkGroup");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simHandleIkGroup_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetIkGroupProperties(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.setIkGroupProperties");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetIkGroupProperties_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),luaToFloat(L,4),nullptr);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetIkElementProperties(luaWrap_lua_State* L)
{ // deprecated on 29.09.2020
    TRACE_LUA_API;
    LUA_START("sim.setIkElementProperties");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int res=checkOneGeneralInputArgument(L,4,lua_arg_number,2,true,true,&errorString);
        if (res>=0)
        {
            float* precision=nullptr;
            float prec[2];
            if (res==2)
            {
                getFloatsFromTable(L,4,2,prec);
                precision=prec;
            }
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,2,true,true,&errorString);
            if (res>=0)
            {
                float* weight=nullptr;
                float w[2];
                if (res==2)
                {
                    getFloatsFromTable(L,5,2,w);
                    weight=w;
                }
                retVal=simSetIkElementProperties_internal(luaToInt(L,1),luaToInt(L,2),luaToInt(L,3),precision,weight,nullptr);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetThreadIsFree(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.setThreadIsFree");

    int retVal=0;
    /*
    int retVal=-1;
    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        bool result=false;
        if (checkInputArguments(L,nullptr,lua_arg_bool,0))
        {
            result=CThreadPool::setThreadFreeMode(luaToBool(L,1));
            if (result)
                retVal=1;
            else
                retVal=0;
        }
        else
            errorString=SIM_ERROR_INVALID_ARGUMENT;
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;
    */
    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTubeRead(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim._tubeRead");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int dataLength;
        char* data=simTubeRead_internal(luaToInt(L,1),&dataLength);
        if (data)
        {
            luaWrap_lua_pushlstring(L,(const char*)data,dataLength);
            delete[] data;
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simTubeOpen(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.tubeOpen");

    int retVal=-1; // Error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_number,0))
    {
        std::string strTmp=luaWrap_lua_tostring(L,2);
        int currentScriptID=getCurrentScriptHandle(L);
        CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
        retVal=App::currentWorld->commTubeContainer->openTube(luaToInt(L,1),strTmp.c_str(),(it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript),luaToInt(L,3));
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTubeClose(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.tubeClose");

    int retVal=-1; // Error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simTubeClose_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTubeWrite(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.tubeWrite");

    int retVal=-1; // Error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
        retVal=simTubeWrite_internal(luaToInt(L,1),data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simTubeStatus(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.tubeStatus");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int readSize;
        int writeSize;
        int status=simTubeStatus_internal(luaToInt(L,1),&readSize,&writeSize);
        if (status>=0)
        {
            luaWrap_lua_pushinteger(L,status);
            luaWrap_lua_pushinteger(L,readSize);
            luaWrap_lua_pushinteger(L,writeSize);
            LUA_END(3);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSendData(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.sendData");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ( (it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript) )
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_string,0,lua_arg_string,0))
        {
            int targetID=luaWrap_lua_tointeger(L,1);
            if ( (targetID<=0)&&(targetID!=sim_handle_all)&&(targetID!=sim_handle_tree)&&(targetID!=sim_handle_chain) )
                errorString=SIM_ERROR_INVALID_TARGET_HANDLE;
            else
            {
                int dataHeader=luaWrap_lua_tointeger(L,2);
                if (dataHeader<0)
                    errorString=SIM_ERROR_INVALID_DATA_HEADER;
                else
                {
                    std::string dataName(luaWrap_lua_tostring(L,3));
                    if ( (dataName.length()==0)||(dataName.find(char(0))!=std::string::npos) )
                        errorString=SIM_ERROR_INVALID_DATA_NAME;
                    else
                    {
                        size_t dataLength;
                        char* data=(char*)luaWrap_lua_tolstring(L,4,&dataLength);
                        if (dataLength<1)
                            errorString=SIM_ERROR_INVALID_DATA;
                        else
                        {
                            // Following are default values:
                            int antennaHandle=sim_handle_self;
                            float actionRadius=100.0f;
                            float emissionAngle1=piValue_f;
                            float emissionAngle2=piValTimes2_f;
                            float persistence=0.0f;
                            bool err=false;
                            int res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                            if (res==2)
                                antennaHandle=luaWrap_lua_tointeger(L,5);
                            err=err||((res!=0)&&(res!=2));
                            if (!err)
                            {
                                if ( (antennaHandle<0)&&(antennaHandle!=sim_handle_default)&&((antennaHandle!=sim_handle_self)||(it->getScriptType()!=sim_scripttype_childscript)) )
                                {
                                    errorString=SIM_ERROR_INVALID_ANTENNA_HANDLE;
                                    err=true;
                                }
                                else
                                {
                                    if (antennaHandle==sim_handle_self)
                                        antennaHandle=it->getObjectIDThatScriptIsAttachedTo_child();
                                    if (antennaHandle!=sim_handle_default)
                                    {
                                        CSceneObject* ant=App::currentWorld->sceneObjects->getObjectFromHandle(antennaHandle);
                                        if (ant==nullptr)
                                        {
                                            errorString=SIM_ERROR_INVALID_ANTENNA_HANDLE;
                                            err=true;
                                        }
                                    }
                                }
                            }
                            if (!err)
                            {
                                int res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,false,&errorString);
                                if (res==2)
                                    actionRadius=luaToFloat(L,6);
                                err=err||((res!=0)&&(res!=2));
                            }
                            if (!err)
                            {
                                int res=checkOneGeneralInputArgument(L,7,lua_arg_number,0,true,false,&errorString);
                                if (res==2)
                                    emissionAngle1=luaToFloat(L,7);
                                err=err||((res!=0)&&(res!=2));
                            }
                            if (!err)
                            {
                                int res=checkOneGeneralInputArgument(L,8,lua_arg_number,0,true,false,&errorString);
                                if (res==2)
                                    emissionAngle2=luaToFloat(L,8);
                                err=err||((res!=0)&&(res!=2));
                            }
                            if (!err)
                            {
                                int res=checkOneGeneralInputArgument(L,9,lua_arg_number,0,true,false,&errorString);
                                if (res==2)
                                    persistence=luaToFloat(L,9);
                                err=err||((res!=0)&&(res!=2));
                            }
                            if (!err)
                            {
                                actionRadius=tt::getLimitedFloat(0.0f,SIM_MAX_FLOAT,actionRadius);
                                emissionAngle1=tt::getLimitedFloat(0.0f,piValue_f,emissionAngle1);
                                emissionAngle2=tt::getLimitedFloat(0.0f,piValTimes2_f,emissionAngle2);
                                persistence=tt::getLimitedFloat(0.0f,99999999999999.9f,persistence);
                                if (persistence==0.0f)
                                    persistence=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())*1.5f/1000000.0f;

                                App::currentWorld->luaScriptContainer->broadcastDataContainer.broadcastData(currentScriptID,targetID,dataHeader,dataName,
                                    float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+persistence,actionRadius,antennaHandle,
                                    emissionAngle1,emissionAngle2,data,(int)dataLength);
                                retVal=1;
                            }
                        }
                    }
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT_OR_CHILD_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simReceiveData(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.receiveData");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if ( (it->getScriptType()==sim_scripttype_mainscript)||(it->getScriptType()==sim_scripttype_childscript) )
    {
        int dataHeader=-1;
        std::string dataName;
        int antennaHandle=sim_handle_self;
        int index=-1;
        bool err=false;
        int res=checkOneGeneralInputArgument(L,1,lua_arg_number,0,true,false,&errorString);
        if (res==2)
        {
            dataHeader=luaWrap_lua_tointeger(L,1);
            if (dataHeader<0)
                dataHeader=-1;
        }
        err=err||((res!=0)&&(res!=2));
        if (!err)
        {
            int res=checkOneGeneralInputArgument(L,2,lua_arg_string,0,true,true,&errorString);
            if (res==2)
            {
                dataName=luaWrap_lua_tostring(L,2);
                if (dataName.length()<1)
                {
                    errorString=SIM_ERROR_INVALID_DATA_NAME;
                    err=true;
                }
            }
            err=err||(res<0);
        }
        if (!err)
        {
            int res=checkOneGeneralInputArgument(L,3,lua_arg_number,0,true,false,&errorString);
            if (res==2)
            {
                antennaHandle=luaWrap_lua_tointeger(L,3);
                if ( (antennaHandle<0)&&(antennaHandle!=sim_handle_default)&&((antennaHandle!=sim_handle_self)||(it->getScriptType()!=sim_scripttype_childscript)) )
                {
                    errorString=SIM_ERROR_INVALID_ANTENNA_HANDLE;
                    err=true;
                }
            }
            err=err||((res!=0)&&(res!=2));
            if (!err)
            {
                if (antennaHandle==sim_handle_self)
                    antennaHandle=it->getObjectIDThatScriptIsAttachedTo_child();
                if (antennaHandle!=sim_handle_default)
                {
                    CSceneObject* ant=App::currentWorld->sceneObjects->getObjectFromHandle(antennaHandle);
                    if (ant==nullptr)
                    {
                        errorString=SIM_ERROR_INVALID_ANTENNA_HANDLE;
                        err=true;
                    }
                }
            }
        }
        if (!err)
        {
            int res=checkOneGeneralInputArgument(L,4,lua_arg_number,0,true,false,&errorString);
            if (res==2)
            {
                index=luaWrap_lua_tointeger(L,4);
                if (index<0)
                    index=-1;
            }
            err=err||((res!=0)&&(res!=2));
        }
        if (!err)
        {
            int theDataHeader;
            int theDataLength;
            int theSenderID;
            std::string theDataName;
            char* data0=App::currentWorld->luaScriptContainer->broadcastDataContainer.receiveData(currentScriptID,float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f,
                    dataHeader,dataName,antennaHandle,theDataLength,index,theSenderID,theDataHeader,theDataName);
            if (data0!=nullptr)
            {
                luaWrap_lua_pushlstring(L,data0,theDataLength);
                luaWrap_lua_pushinteger(L,theSenderID);
                luaWrap_lua_pushinteger(L,theDataHeader);
                luaWrap_lua_pushstring(L,theDataName.c_str());
                LUA_END(4);
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT_OR_CHILD_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simFollowPath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.followPath");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (!(CThreadPool::getSimulationStopRequested()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L))))
        { // Important to first check if we are supposed to leave the thread!!
            if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
            { // Those are the arguments that are always required! (the rest can be ignored or set to nil!
                int objID=luaWrap_lua_tointeger(L,1);
                int pathID=luaWrap_lua_tointeger(L,2);
                float posOnPath=luaToFloat(L,4);
                int positionAndOrOrientation=abs(luaToInt(L,3));
                if (positionAndOrOrientation==0)
                    positionAndOrOrientation=1;
                float maxVelocity=luaToFloat(L,5);
                CSceneObject* object=App::currentWorld->sceneObjects->getObjectFromHandle(objID);
                CPath* path=App::currentWorld->sceneObjects->getPathFromHandle(pathID);
                float accel=0.0f; // means infinite accel!! (default value)
                bool foundError=false;
                if ((!foundError)&&(object==nullptr))
                {
                    errorString=SIM_ERROR_OBJECT_INEXISTANT;
                    foundError=true;
                }
                if ((!foundError)&&(path==nullptr))
                {
                    errorString=SIM_ERROR_PATH_INEXISTANT;
                    foundError=true;
                }
                if (!foundError)
                {
                    if (path->pathContainer->getAttributes()&sim_pathproperty_closed_path)
                    {
                        if (posOnPath<0.0f)
                            posOnPath=0.0f;
                    }
                    else
                        posOnPath=tt::getLimitedFloat(0.0f,1.0f,posOnPath);
                }

                // Now check the optional arguments:
                int res;
                if (!foundError) // Accel argument:
                {
                    res=checkOneGeneralInputArgument(L,6,lua_arg_number,0,true,true,&errorString);
                    if (res==2)
                    { // get the data
                        accel=fabs(luaToFloat(L,6));
                    }
                    foundError=(res==-1);
                }
                if (!foundError)
                { // do the job here!
                    float bezierPathLength=path->pathContainer->getBezierVirtualPathLength();
                    double pos=posOnPath*bezierPathLength;
                    float vel=0.0f;
                    CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                    float lastTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+threadData->usedMovementTime;
                    bool movementFinished=(bezierPathLength==0.0f);
                    float dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f; // this is the time left if we leave here
                    bool err=false;
                    while (!movementFinished)
                    {
                        if ((App::currentWorld->sceneObjects->getObjectFromHandle(objID)!=object)||(App::currentWorld->sceneObjects->getPathFromHandle(pathID)!=path) ) // make sure the objects are still valid (running in a thread)
                        {
                            dt=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
                            break;
                        }

                        float currentTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f;
                        dt=currentTime-lastTime;
                        lastTime=currentTime;
                        if (accel==0.0f)
                        { // Means infinite acceleration
                            path->pathContainer->handlePath_keepObjectUnchanged(dt,pos,vel,maxVelocity,1.0f,false,true);
                        }
                        else
                        {
                            path->pathContainer->handlePath_keepObjectUnchanged(dt,pos,vel,maxVelocity,accel,false,false);
                        }

                        // Now check if we are within tolerances:
                        if ( ((fabs(pos-bezierPathLength)<=0.00001f)&&(maxVelocity>=0.0f)) || ((fabs(pos-0.0f)<=0.00001f)&&(maxVelocity<=0.0f)) )
                            movementFinished=true;

                        // Set the new configuration of the object:
                        float ll=float(pos/bezierPathLength);
                        C7Vector newAbs;
                        if (path->pathContainer->getTransformationOnBezierCurveAtNormalizedVirtualDistance(ll,newAbs))
                        {
                            newAbs=path->getCumulativeTransformation()*newAbs;
                            C7Vector parentInv(object->getFullParentCumulativeTransformation().getInverse());
                            C7Vector currAbs(object->getCumulativeTransformation());
                            if ((positionAndOrOrientation&1)==0)
                                newAbs.X=currAbs.X;
                            if ((positionAndOrOrientation&2)==0)
                                newAbs.Q=currAbs.Q;
                            object->setLocalTransformation(parentInv*newAbs);
                        }
                        else
                            movementFinished=true;

                        if (!movementFinished)
                        {
                            CThreadPool::switchBackToPreviousThread();
                            if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                            {
                                errorString="@yield"; // yield will be triggered at end of this function
                                err=true;
                                break;
                            }
                        }
                    }
                    if (!err)
                    {
                        // The movement finished. Now add the time used:
                        threadData->usedMovementTime=float(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0f-dt;
                        luaWrap_lua_pushnumber(L,dt); // success
                        LUA_END(1);
                    }
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetDataOnPath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getDataOnPath");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        int auxFlags;
        float auxChannels[4];
        if (simGetDataOnPath_internal(luaToInt(L,1),luaToFloat(L,2),0,&auxFlags,auxChannels)==1)
        {
            luaWrap_lua_pushinteger(L,auxFlags);
            pushFloatTableOntoStack(L,4,auxChannels);
            LUA_END(2);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetPositionOnPath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getPositionOnPath");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float coord[3];
        if (simGetPositionOnPath_internal(luaToInt(L,1),luaToFloat(L,2),coord)==1)
        {
            pushFloatTableOntoStack(L,3,coord);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetOrientationOnPath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getOrientationOnPath");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        float coord[3];
        if (simGetOrientationOnPath_internal(luaToInt(L,1),luaToFloat(L,2),coord)==1)
        {
            pushFloatTableOntoStack(L,3,coord);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetClosestPositionOnPath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getClosestPositionOnPath");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,3))
    {
        float coord[3];
        getFloatsFromTable(L,2,3,coord);
        float dist=0.0f;
        if (simGetClosestPositionOnPath_internal(luaToInt(L,1),coord,&dist)!=-1)
        {
            luaWrap_lua_pushnumber(L,dist);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simGetPathPosition(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getPathPosition");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float pathVal[1];
        if (simGetPathPosition_internal(luaToInt(L,1),pathVal)!=-1)
        {
            luaWrap_lua_pushnumber(L,pathVal[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetPathPosition(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.setPathPosition");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=simSetPathPosition_internal(luaToInt(L,1),luaToFloat(L,2));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetPathLength(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getPathLength");

    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        float pathLen[1];
        if (simGetPathLength_internal(luaToInt(L,1),pathLen)!=-1)
        {
            luaWrap_lua_pushnumber(L,pathLen[0]);
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simInsertPathCtrlPoints(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.insertPathCtrlPoints");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int pathHandle=luaToInt(L,1);
        int options=luaToInt(L,2);
        int startIndex=luaToInt(L,3);
        int ptCnt=luaToInt(L,4);
        int floatOrIntCountPerPoint=11;
        if (options&2)
            floatOrIntCountPerPoint=16;

        int res=checkOneGeneralInputArgument(L,5,lua_arg_number,ptCnt*floatOrIntCountPerPoint,false,false,&errorString);
        if (res==2)
        {
            float* data=new float[ptCnt*floatOrIntCountPerPoint];
            getFloatsFromTable(L,5,ptCnt*floatOrIntCountPerPoint,data);
            for (int i=0;i<ptCnt;i++)
                ((int*)(data+floatOrIntCountPerPoint*i+8))[0]=int(data[floatOrIntCountPerPoint*i+8]+0.5f);
            if (options&2)
            {
                for (int i=0;i<ptCnt;i++)
                    ((int*)(data+floatOrIntCountPerPoint*i+11))[0]=int(data[floatOrIntCountPerPoint*i+11]+0.5f);
            }
            retVal=simInsertPathCtrlPoints_internal(pathHandle,options,startIndex,ptCnt,data);
            delete[] data;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCutPathCtrlPoints(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.cutPathCtrlPoints");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
    {
        int pathHandle=luaToInt(L,1);
        int startIndex=luaToInt(L,2);
        int ptCnt=luaToInt(L,3);
        retVal=simCutPathCtrlPoints_internal(pathHandle,startIndex,ptCnt);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simCreatePath(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.createPath");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
    {
        int attribs=luaToInt(L,1);
        int* intP=nullptr;
        float* floatP=nullptr;
        int intParams[3];
        float floatParams[3];
        float* color=nullptr;
        float c[12];
        int res=checkOneGeneralInputArgument(L,2,lua_arg_number,3,true,true,&errorString);
        if (res>=0)
        {
            if (res==2)
            {
                getIntsFromTable(L,2,3,intParams);
                intP=intParams;
            }
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,3,true,true,&errorString);
            if (res>=0)
            {
                if (res==2)
                {
                    getFloatsFromTable(L,3,3,floatParams);
                    floatP=floatParams;
                }
                res=checkOneGeneralInputArgument(L,4,lua_arg_number,12,true,true,&errorString);
                if (res>=0)
                {
                    if (res==2)
                    {
                        getFloatsFromTable(L,4,12,c);
                        color=c;
                    }
                    retVal=simCreatePath_internal(attribs,intP,floatP,color);
                }
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetScriptExecutionCount(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getScriptExecutionCount");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,it->getNumberOfPasses());
    LUA_END(1);
}

int _simIsScriptExecutionThreaded(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.isScriptExecutionThreaded");

    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(currentScriptID);
    int retVal=0;
    if ((it!=nullptr)&&it->getThreadedExecution())
        retVal=1;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simIsScriptRunningInThread(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.isScriptRunningInThread");

    int retVal=1;
    if (VThread::isCurrentThreadTheMainSimulationThread())
        retVal=0;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSetThreadResumeLocation(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.setThreadResumeLocation");

    int retVal=-1;
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
    {
        if (CThreadPool::setThreadResumeLocation(luaWrap_lua_tointeger(L,1),luaWrap_lua_tointeger(L,2)))
            retVal=1;
        else
            retVal=0;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simResumeThreads(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.resumeThreads");

    int retVal=-1;
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    if (it->getScriptType()==sim_scripttype_mainscript)
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            int loc=luaWrap_lua_tointeger(L,1);

            int startT=VDateTime::getTimeInMs();
            retVal=App::currentWorld->luaScriptContainer->handleCascadedScriptExecution(sim_scripttype_childscript|sim_scripttype_threaded_old,loc,nullptr,nullptr,nullptr);
            // Following line important: when erasing a running threaded script object, with above cascaded
            // call, the thread will never resume nor be able to end. Next line basically runs all
            // that were not yet ran:
            retVal+=CThreadPool::handleAllThreads_withResumeLocation(loc);
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simLaunchThreadedChildScripts(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.launchThreadedChildScripts");

    int retVal=-1; // means error
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_noAddOnsNorSandbox(currentScriptID);
    if (it!=nullptr)
    {
        if (it->getScriptType()==sim_scripttype_mainscript)
        {
            int startT=VDateTime::getTimeInMs();
            retVal=App::currentWorld->luaScriptContainer->handleCascadedScriptExecution(sim_scripttype_childscript|sim_scripttype_threaded_old,sim_scriptthreadresume_launch,nullptr,nullptr,nullptr);
        }
        else
            errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_MAIN_SCRIPT;
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetThreadId(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getThreadId");

    int retVal=simGetThreadId_internal();

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simGetUserParameter(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.getUserParameter");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
        {
            handle=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(handle);
            handle=it->getObjectIDThatScriptIsAttachedTo();
        }
        bool returnString=false;
        int ret=checkOneGeneralInputArgument(L,3,lua_arg_bool,0,true,false,&errorString);
        if ((ret==0)||(ret==2))
        {
            if (ret==2)
                returnString=luaToBool(L,3);
            std::string parameterName(luaWrap_lua_tostring(L,2));
            int l;
            char* p=simGetUserParameter_internal(handle,parameterName.c_str(),&l);
            if (p!=nullptr)
            {
                std::string a;
                a.assign(p,l);
                if (returnString)
                    luaWrap_lua_pushlstring(L,a.c_str(),a.length());
                else
                    pushCorrectTypeOntoLuaStack(L,a);
                simReleaseBuffer_internal(p);
                LUA_END(1);
            }
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSetUserParameter(luaWrap_lua_State* L)
{ // deprecated on 01.10.2020
    TRACE_LUA_API;
    LUA_START("sim.setUserParameter");

    int retVal=-1;// error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0,lua_arg_string,0))
    {
        int handle=luaWrap_lua_tointeger(L,1);
        if (handle==sim_handle_self)
        {
            handle=getCurrentScriptHandle(L);
            CLuaScriptObject* it=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(handle);
            handle=it->getObjectIDThatScriptIsAttachedTo();
        }
        std::string parameterName(luaWrap_lua_tostring(L,2));
        size_t parameterValueLength;
        char* parameterValue=(char*)luaWrap_lua_tolstring(L,3,&parameterValueLength);
        retVal=simSetUserParameter_internal(handle,parameterName.c_str(),parameterValue,(int)parameterValueLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _genericFunctionHandler_old(luaWrap_lua_State* L,CLuaCustomFunction* func)
{ // deprecated around 2015
    TRACE_LUA_API;
    // We first read all arguments from the stack
    std::vector<char> inBoolVector;
    std::vector<int> inIntVector;
    std::vector<float> inFloatVector;
    std::vector<double> inDoubleVector;
    std::vector<std::string> inStringVector;
    std::vector<std::string> inCharVector;
    std::vector<int> inInfoVector;
    for (int i=0;i<int(func->inputArgTypes.size());i++)
    {
        if (!readCustomFunctionDataFromStack(L,i+1,func->inputArgTypes[i],inBoolVector,inIntVector,inFloatVector,inDoubleVector,inStringVector,inCharVector,inInfoVector))
            break;
    }

    // Now we retrieve the object ID this script might be attached to:
    int currentScriptID=getCurrentScriptHandle(L);
    CLuaScriptObject* itObj=App::currentWorld->luaScriptContainer->getScriptFromID_alsoAddOnsAndSandbox(currentScriptID);
    int linkedObject=-1;
    if (itObj->getScriptType()==sim_scripttype_childscript)
    {
        CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(itObj->getObjectIDThatScriptIsAttachedTo_child());
        if (obj!=nullptr)
            linkedObject=obj->getObjectHandle();
    }
    if (itObj->getScriptType()==sim_scripttype_customizationscript)
    {
        CSceneObject* obj=App::currentWorld->sceneObjects->getObjectFromHandle(itObj->getObjectIDThatScriptIsAttachedTo_customization());
        if (obj!=nullptr)
            linkedObject=obj->getObjectHandle();
    }
    // We prepare the callback structure:
    SLuaCallBack* p=new SLuaCallBack;
    p->objectID=linkedObject;
    p->scriptID=currentScriptID;
    p->inputBool=nullptr;
    p->inputInt=nullptr;
    p->inputFloat=nullptr;
    p->inputDouble=nullptr;
    p->inputChar=nullptr;
    p->inputCharBuff=nullptr;
    p->inputArgCount=0;
    p->inputArgTypeAndSize=nullptr;
    p->outputBool=nullptr;
    p->outputInt=nullptr;
    p->outputFloat=nullptr;
    p->outputDouble=nullptr;
    p->outputChar=nullptr;
    p->outputCharBuff=nullptr;
    p->outputArgCount=0;
    p->outputArgTypeAndSize=nullptr;
    p->waitUntilZero=0;
    // Now we prepare the input buffers:
    p->inputBool=new unsigned char[inBoolVector.size()];
    p->inputInt=new int[inIntVector.size()];
    p->inputFloat=new float[inFloatVector.size()];
    p->inputDouble=new double[inDoubleVector.size()];
    int charCnt=0;
    for (size_t k=0;k<inStringVector.size();k++)
        charCnt+=(int)inStringVector[k].length()+1; // terminal 0
    p->inputChar=new char[charCnt];

    int charBuffCnt=0;
    for (size_t k=0;k<inCharVector.size();k++)
        charBuffCnt+=(int)inCharVector[k].length();
    p->inputCharBuff=new char[charBuffCnt];

    p->inputArgCount=(int)inInfoVector.size()/2;
    p->inputArgTypeAndSize=new int[inInfoVector.size()];
    // We fill the input buffers:
    for (int k=0;k<int(inBoolVector.size());k++)
        p->inputBool[k]=inBoolVector[k];
    for (int k=0;k<int(inIntVector.size());k++)
        p->inputInt[k]=inIntVector[k];
    for (int k=0;k<int(inFloatVector.size());k++)
        p->inputFloat[k]=inFloatVector[k];
    for (int k=0;k<int(inDoubleVector.size());k++)
        p->inputDouble[k]=inDoubleVector[k];
    charCnt=0;
    for (int k=0;k<int(inStringVector.size());k++)
    {
        for (int l=0;l<int(inStringVector[k].length());l++)
            p->inputChar[charCnt+l]=inStringVector[k][l];
        charCnt+=(int)inStringVector[k].length();
        // terminal 0:
        p->inputChar[charCnt]=0;
        charCnt++;
    }

    charBuffCnt=0;
    for (int k=0;k<int(inCharVector.size());k++)
    {
        for (int l=0;l<int(inCharVector[k].length());l++)
            p->inputCharBuff[charBuffCnt+l]=inCharVector[k][l];
        charBuffCnt+=(int)inCharVector[k].length();
    }

    for (int k=0;k<int(inInfoVector.size());k++)
        p->inputArgTypeAndSize[k]=inInfoVector[k];

    // Now we can call the callback:
    func->callBackFunction_old(p);

    bool dontDeleteStructureYet=false;
    while (p->waitUntilZero!=0)
    { // backward compatibility (for real threads)
        if (!CThreadPool::switchBackToPreviousThread())
            break;
        if (CThreadPool::getSimulationStopRequestedAndActivated())
        { // give a chance to the c app to set the waitUntilZero to zero! (above turns true only 1-2 secs after the stop request arrived)
            // Following: the extension module might still write 0 into that position to signal "no more waiting" in
            // case this while loop got interrupted by a stop request.
            dontDeleteStructureYet=true;
            break;
        }
    }

    // We first delete the input buffers:
    delete[] p->inputBool;
    delete[] p->inputInt;
    delete[] p->inputFloat;
    delete[] p->inputDouble;
    delete[] p->inputChar;
    delete[] p->inputCharBuff;
    delete[] p->inputArgTypeAndSize;
    // Now we have to build the returned data onto the stack:
    if (p->outputArgCount!=0)
    {
        int boolPt=0;
        int intPt=0;
        int floatPt=0;
        int doublePt=0;
        int stringPt=0;
        int stringBuffPt=0;
        for (int i=0;i<p->outputArgCount;i++)
        {
            writeCustomFunctionDataOntoStack(L,p->outputArgTypeAndSize[2*i+0],p->outputArgTypeAndSize[2*i+1],
                p->outputBool,boolPt,
                p->outputInt,intPt,
                p->outputFloat,floatPt,
                p->outputDouble,doublePt,
                p->outputChar,stringPt,
                p->outputCharBuff,stringBuffPt);
        }
    }
    // We now delete the output buffers:
    delete[] p->outputBool;
    delete[] p->outputInt;
    delete[] p->outputFloat;
    delete[] p->outputDouble;
    delete[] p->outputChar;
    delete[] p->outputCharBuff;
    delete[] p->outputArgTypeAndSize;
    // And we return the number of arguments:
    int outputArgCount=p->outputArgCount;
    if (dontDeleteStructureYet)
    {   // We cannot yet delete the structure because an extension module might still write '0' into
        // p->waitUntilZero!! We delete the structure at the end of the simulation.
        App::currentWorld->luaScriptContainer->addCallbackStructureObjectToDestroyAtTheEndOfSimulation_old(p);
    }
    else
        delete p;
    if (outputArgCount==0)
    {
        luaWrap_lua_pushnil(L);
        outputArgCount=1;
    }
    return(outputArgCount);
}



/*
int _simWait(luaWrap_lua_State* L)
{ // can only be called from a script running in a thread!!
    TRACE_LUA_API;
    LUA_START("sim.wait");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (checkInputArguments(L,&errorString,lua_arg_number,0))
        {
            float deltaTime=luaToFloat(L,1);
            int res=checkOneGeneralInputArgument(L,2,lua_arg_bool,0,true,false,&errorString);
            if (res!=-1)
            {
                bool simulationTime=true;
                if (res==2)
                    simulationTime=luaToBool(L,2);
                if (!simulationTime)
                { // real-time wait
                    CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                    int startTime=VDateTime::getTimeInMs();
                    bool err=false;
                    while (true)
                    {
                        float diff=float(VDateTime::getTimeDiffInMs(startTime))/1000.0f;
                        if (diff>=deltaTime)
                            break;
                        CThreadPool::switchBackToPreviousThread();
                        if (CThreadPool::getSimulationStopRequestedAndActivated())
                        {
                            errorString="@yield"; // yield will be triggered at end of this function
                            err=true;
                            break;
                        }
                    }
                    threadData->usedMovementTime=0.0f; // important!
                    if (!err)
                    {
                        luaWrap_lua_pushnumber(L,0.0f); // success (deltaTime left)
                        LUA_END(1);
                    }
                }
                else
                { // simulation time wait
                    CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                    float startTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f+threadData->usedMovementTime;
                    float overshootTime=0.0f;
                    bool err=false;
                    while (true)
                    {
                        float currentTime=float(App::currentWorld->simulation->getSimulationTime_us())/1000000.0f;
                        float diff=currentTime-startTime;
                        if (diff>=deltaTime)
                        {
                            overshootTime=diff-deltaTime; // this is the "overshoot" time!
                            break;
                        }
                        CThreadPool::switchBackToPreviousThread();
//                        luaWrap_lua_yield(L,0); // does a long jump and never returns
                        if (CThreadPool::getSimulationStopRequestedAndActivated())
                        {
                            errorString="@yield"; // yield will be triggered at end of this function
                            err=true;
                            break;
                        }
                    }
                    threadData->usedMovementTime=overshootTime; // important!
                    if (!err)
                    {
                        luaWrap_lua_pushnumber(L,-overshootTime); // success (deltaTime left)
                        LUA_END(1);
                    }
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simWaitForSignal(luaWrap_lua_State* L)
{ // can only be called from a script running in a thread!!
    TRACE_LUA_API;
    LUA_START("sim.waitForSignal");

    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (checkInputArguments(L,&errorString,lua_arg_string,0))
        {
            std::string signalName(luaWrap_lua_tostring(L,1));
            int intVal;
            bool signalPresent=false;

            CSignalContainer* sigCont=nullptr;
            sigCont=App::currentWorld->signalContainer;

            while (!signalPresent)
            {
                if (sigCont->getIntegerSignal(signalName.c_str(),intVal))
                {
                    luaWrap_lua_pushinteger(L,intVal);
                    signalPresent=true;
                    LUA_END(1);
                }
                float floatVal;
                if ( (!signalPresent)&&(sigCont->getFloatSignal(signalName.c_str(),floatVal)) )
                {
                    luaWrap_lua_pushnumber(L,floatVal);
                    signalPresent=true;
                    LUA_END(1);
                }
                std::string strVal;
                if ( (!signalPresent)&&(sigCont->getStringSignal(signalName.c_str(),strVal)) )
                {
                    luaWrap_lua_pushstring(L,strVal.c_str());
                    signalPresent=true;
                    LUA_END(1);
                }
                if (!signalPresent)
                {
                    CThreadPool::switchBackToPreviousThread();
                    if (CThreadPool::getSimulationStopRequestedAndActivated())
                    {
                        errorString="@yield"; // yield will be triggered at end of this function
                        break;
                    }
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSerialRead(luaWrap_lua_State* L)
{
    TRACE_LUA_API;
    LUA_START("sim.serialRead");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_bool,0))
    {
        bool blocking=luaToBool(L,3);
        std::string closingString="";
        float timeOut=0.0f;
        unsigned int maxLength=(unsigned int)luaToInt(L,2);
        std::string fullDataRead;
        bool err=false;
        if (blocking)
        {
            if (!VThread::isCurrentThreadTheMainSimulationThread())
            {
                int res=checkOneGeneralInputArgument(L,4,lua_arg_string,0,true,true,&errorString);
                if (res==2)
                {
                    size_t dataLength;
                    char* data=(char*)luaWrap_lua_tolstring(L,4,&dataLength);
                    closingString.assign(data,dataLength);
                }
                if (res!=-1)
                {
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                    if (res==2)
                        timeOut=luaToFloat(L,5);
                    if (res!=-1)
                    {
                        int startTime=VDateTime::getTimeInMs();
                        std::string leftOver=retrieveSerialPortLeftOver(luaToInt(L,1));
                        fullDataRead=leftOver;
                        unsigned int closingCheckedIndex=0;
                        char* data=new char[maxLength-leftOver.length()];
                        unsigned int csl=(unsigned int)closingString.length();
                        while (true)
                        {
                            // 1. Check in what we already have:
                            if ( (csl>0)&&(closingCheckedIndex+csl<=fullDataRead.length()) )
                            {
                                bool foundTheString=false;
                                for (unsigned int j=closingCheckedIndex;j<=fullDataRead.length()-csl;j++)
                                {
                                    bool same=true;
                                    for (unsigned int i=0;i<csl;i++)
                                    {
                                        if (fullDataRead[j+i]!=closingString[i])
                                        {
                                            same=false;
                                            break;
                                        }
                                    }
                                    if (same)
                                    {
                                        std::string rest(fullDataRead.begin()+j+csl,fullDataRead.end());
                                        fullDataRead.assign(fullDataRead.begin(),fullDataRead.begin()+j);
                                        insertSerialPortLeftOver(luaToInt(L,1),rest);
                                        foundTheString=true;
                                        break;
                                    }
                                }
                                if (foundTheString)
                                    break;
                                closingCheckedIndex=(unsigned int)fullDataRead.length()-csl+1;
                            }
                            // 2. Did we already read enough?
                            if (fullDataRead.length()>=maxLength)
                            { // yes
                                if (fullDataRead.length()==maxLength)
                                    break;
                                std::string rest(fullDataRead.begin()+maxLength,fullDataRead.end());
                                fullDataRead.assign(fullDataRead.begin(),fullDataRead.begin()+maxLength);
                                insertSerialPortLeftOver(luaToInt(L,1),rest);
                                break;
                            }
                            else
                            { // no
                                int nb=simSerialRead_internal(luaToInt(L,1),data,(int)maxLength-(int)fullDataRead.length());
                                if (nb<0)
                                    break; // error
                                std::string partString(data,nb);
                                fullDataRead+=partString;
                            }

                            bool leaveHere=false;
                            if (maxLength>fullDataRead.length())
                            {
                                CThreadPool::switchBackToPreviousThread();
                                if (CThreadPool::getSimulationStopRequestedAndActivated())
                                {
                                    errorString="@yield"; // yield will be triggered at end of this function
                                    err=true;
                                    leaveHere=true;
                                }
                            }
                            if ( (timeOut>0.0000001f)&&((float(VDateTime::getTimeDiffInMs(startTime))/1000.0f)>timeOut) )
                                leaveHere=true;

                            if (leaveHere)
                            {
                                insertSerialPortLeftOver(luaToInt(L,1),fullDataRead);
                                fullDataRead.clear();
                                break;
                            }
                        }
                        delete[] data;
                    }
                }
            }
            else
                errorString=SIM_ERROR_BLOCKING_OPERATION_ONLY_FROM_THREAD;
        }
        else
        {
            std::string leftOver=retrieveSerialPortLeftOver(luaToInt(L,1));
            char* data=new char[maxLength-leftOver.length()];
            int nb=simSerialRead_internal(luaToInt(L,1),data,(int)maxLength-(int)leftOver.length());
            if (nb>0)
            { // no error
                std::string nextData(data,nb);
                fullDataRead=leftOver+nextData;
            }
            delete[] data;
        }

        if ( (!err)&&(fullDataRead.length()>0) )
        {
            luaWrap_lua_pushlstring(L,fullDataRead.c_str(),fullDataRead.length());
            LUA_END(1);
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simSerialPortOpen(luaWrap_lua_State* L)
{ // DEPRECATED For backward compatibility (10/04/2012)
    TRACE_LUA_API;
    LUA_START("simSerialPortOpen");

    int retVal=-1; // means error
#ifdef SIM_WITH_SERIAL
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0))
        retVal=App::worldContainer->serialPortContainer->serialPortOpen_old(true,luaToInt(L,1),luaToInt(L,2));
#endif

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialPortClose(luaWrap_lua_State* L)
{ // DEPRECATED For backward compatibility (10/04/2012)
    TRACE_LUA_API;
    LUA_START("simSerialPortClose");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0))
        retVal=simSerialPortClose_internal(luaToInt(L,1));

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialPortSend(luaWrap_lua_State* L)
{ // DEPRECATED For backward compatibility (10/04/2012)
    TRACE_LUA_API;
    LUA_START("simSerialPortSend");

    int retVal=-1; // means error
    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_string,0))
    {
        size_t dataLength;
        char* data=(char*)luaWrap_lua_tolstring(L,2,&dataLength);
        retVal=simSerialPortSend_internal(luaToInt(L,1),data,(int)dataLength);
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal);
    LUA_END(1);
}

int _simSerialPortRead(luaWrap_lua_State* L)
{ // DEPRECATED For backward compatibility (10/04/2012)
    TRACE_LUA_API;
    LUA_START("simSerialPortRead");

    if (checkInputArguments(L,&errorString,lua_arg_number,0,lua_arg_number,0,lua_arg_bool,0))
    {
        bool blocking=luaToBool(L,3);
        std::string closingString="";
        float timeOut=0.0f;
        bool err=false;
        if (blocking)
        {
            if (!VThread::isCurrentThreadTheMainSimulationThread())
            {
                int res=checkOneGeneralInputArgument(L,4,lua_arg_string,0,true,true,&errorString);
                if (res==2)
                {
                    size_t dataLength;
                    char* data=(char*)luaWrap_lua_tolstring(L,4,&dataLength);
                    closingString.assign(data,dataLength);
                }
                if (res!=-1)
                {
                    res=checkOneGeneralInputArgument(L,5,lua_arg_number,0,true,false,&errorString);
                    if (res==2)
                        timeOut=luaToFloat(L,5);
                    if (res!=-1)
                        err=true;
                }
                else
                    err=true;
            }
            else
            {
                errorString=SIM_ERROR_BLOCKING_OPERATION_ONLY_FROM_THREAD;
                err=true;
            }
        }
        if (!err)
        {
            int maxLength=luaToInt(L,2);
            char* data=new char[maxLength];
            int dataRead=0;
            bool err=false;
            if (blocking)
            {
                int startTime=VDateTime::getTimeInMs();
                while (maxLength>dataRead)
                {
                    int nb=simSerialPortRead_internal(luaToInt(L,1),data+dataRead,maxLength-dataRead);
                    if (nb<0)
                        break; // error
                    dataRead+=nb;
                    int csl=int(closingString.length());
                    if ( (csl!=0)&&(dataRead>=csl) )
                    { // Break if we find the closing signature:
                        bool same=true;
                        for (int i=0;i<csl;i++)
                        {
                            if (data[dataRead-csl+i]!=closingString[i])
                            {
                                same=false;
                                break;
                            }
                        }
                        if (same)
                        {
                            dataRead-=csl;
                            break;
                        }
                    }
                    if (maxLength>dataRead)
                    {
                        CThreadPool::switchBackToPreviousThread();
                        if (CThreadPool::getSimulationStopRequestedAndActivated())
                        {
                            errorString="@yield"; // yield will be triggered at end of this function
                            err=true;
                            break;
                        }
                    }
                    if ( (timeOut>0.0000001f)&&((float(VDateTime::getTimeDiffInMs(startTime))/1000.0f)>timeOut) )
                        break;
                }
            }
            else
                dataRead=simSerialPortRead_internal(luaToInt(L,1),data,maxLength);
            if ( (dataRead>0)&&(!err) )
            {
                luaWrap_lua_pushlstring(L,(const char*)data,dataRead);
                delete[] data;
                LUA_END(1);
            }
            delete[] data;
        }
    }

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    LUA_END(0);
}

int _simRMLMoveToJointPositions(luaWrap_lua_State* L)
{ // DEPRECATED on 08.09.2020
    TRACE_LUA_API;
    LUA_START_NO_CSIDE_ERROR("sim.rmlMoveToJointPositions");

    int retVal=-1; //error
    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (!( (!luaWrap_lua_istable(L,1))||(luaWrap_lua_rawlen(L,1)<1) ))
        {
            int dofs=(int)luaWrap_lua_rawlen(L,1);
            int flags=-1;
            int* jointHandles=new int[dofs];
            double* currentVel=new double[dofs];
            double* currentAccel=new double[dofs];
            double* maxVel=new double[dofs];
            double* maxAccel=new double[dofs];
            double* maxJerk=new double[dofs];
            double* targetPos=new double[dofs];
            double* targetVel=new double[dofs];
            int* direction=new int[dofs];
            for (int i=0;i<dofs;i++)
            {
                currentVel[i]=0.0;
                currentAccel[i]=0.0;
                maxVel[i]=0.0;
                maxAccel[i]=0.0;
                maxJerk[i]=0.0;
                targetPos[i]=0.0;
                targetVel[i]=0.0;
                direction[i]=0;
            }
            bool argError=false;

            // jointHandles
            int res=checkOneGeneralInputArgument(L,1,lua_arg_number,dofs,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    getIntsFromTable(L,1,dofs,jointHandles);
            }
            if (!argError)
            { // check if all joint handles are ok:
                for (int i=0;i<dofs;i++)
                {
                    CJoint* act=App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i]);
                    if (act==nullptr)
                        argError=true;
                }
                if (argError)
                    errorString=SIM_ERROR_FOUND_INVALID_HANDLES;
            }

            // flags
            res=checkOneGeneralInputArgument(L,2,lua_arg_number,0,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    flags=luaToInt(L,2);
            }

            // currentVel
            res=checkOneGeneralInputArgument(L,3,lua_arg_number,dofs,false,true,&errorString);
            if ((!argError)&&(res<1))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,3,dofs,currentVel);
            }

            // currentAccel
            res=checkOneGeneralInputArgument(L,4,lua_arg_number,dofs,false,true,&errorString);
            if ((!argError)&&(res<1))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,4,dofs,currentAccel);
            }

            // maxVel
            res=checkOneGeneralInputArgument(L,5,lua_arg_number,dofs,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,5,dofs,maxVel);
            }

            // maxAccel
            res=checkOneGeneralInputArgument(L,6,lua_arg_number,dofs,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,6,dofs,maxAccel);
            }

            // maxJerk
            res=checkOneGeneralInputArgument(L,7,lua_arg_number,dofs,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,7,dofs,maxJerk);
            }

            // targetPos
            res=checkOneGeneralInputArgument(L,8,lua_arg_number,dofs,false,false,&errorString);
            if ((!argError)&&(res<2))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,8,dofs,targetPos);
            }

            // targetVel
            res=checkOneGeneralInputArgument(L,9,lua_arg_number,dofs,false,true,&errorString);
            if ((!argError)&&(res<1))
                argError=true;
            else
            {
                if (res==2)
                    getDoublesFromTable(L,9,dofs,targetVel);
            }

            res=checkOneGeneralInputArgument(L,10,lua_arg_number,dofs,true,true,&errorString);
            if ((!argError)&&(res<0))
                argError=true;
            else
            {
                if (res==2)
                    getIntsFromTable(L,10,dofs,direction);
            }

            if (!argError)
            {
                unsigned char auxData[9];
                auxData[0]=1;
                double* currentPosVelAccel=new double[3*dofs];
                double* newPosVelAccel=new double[3*dofs];
                double* maxVelAccelJerk=new double[3*dofs];
                double* targetPosVel=new double[2*dofs];
                unsigned char* selection=new unsigned char[dofs];
                for (int i=0;i<dofs;i++)
                {
                    CJoint* act=App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i]);
                    if (act!=nullptr) // should always pass!
                    {
                        selection[i]=1;
                        float ps=act->getPosition();
                        if ( (act->getJointType()==sim_joint_revolute_subtype)&&act->getPositionIsCyclic() )
                        {
                            if (direction[i]==0)
                            { // smallest movement:
                                float dx=targetPos[i]-ps;
                                while (dx>=piValTimes2_f)
                                {
                                    ps+=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                                while (dx<0.0f)
                                {
                                    ps-=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                                float b=ps+piValTimes2_f;
                                if (fabs(targetPos[i]-b)<fabs(targetPos[i]-ps))
                                    ps=b;
                            }
                            if (direction[i]>0)
                            { // positive direction:
                                float dx=targetPos[i]-ps;
                                while (dx<piValTimes2_f*float(direction[i]-1))
                                {
                                    ps-=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                                while (dx>=piValTimes2_f*float(direction[i]))
                                {
                                    ps+=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                            }
                            if (direction[i]<0)
                            { // negative direction:
                                float dx=targetPos[i]-ps;
                                while (dx>-piValTimes2_f*float(-direction[i]-1))
                                {
                                    ps+=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                                while (dx<=-piValTimes2_f*float(-direction[i]))
                                {
                                    ps-=piValTimes2_f;
                                    dx=targetPos[i]-ps;
                                }
                            }
                            currentPosVelAccel[0*dofs+i]=ps;
                        }
                        else
                            currentPosVelAccel[0*dofs+i]=ps;
                    }
                    else
                        selection[i]=0;

                    currentPosVelAccel[1*dofs+i]=currentVel[i];
                    currentPosVelAccel[2*dofs+i]=currentAccel[i];

                    maxVelAccelJerk[0*dofs+i]=maxVel[i];
                    maxVelAccelJerk[1*dofs+i]=maxAccel[i];
                    maxVelAccelJerk[2*dofs+i]=maxJerk[i];

                    targetPosVel[0*dofs+i]=targetPos[i];
                    targetPosVel[1*dofs+i]=targetVel[i];

                    selection[i]=1;
                }

                bool movementFinished=false;


                int rmlHandle=simRMLPos_internal(dofs,0.0001,flags,currentPosVelAccel,maxVelAccelJerk,selection,targetPosVel,nullptr);
                while (!movementFinished)
                {
                    double dt=double(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0;
                    int rmlRes=simRMLStep_internal(rmlHandle,dt,newPosVelAccel,auxData,nullptr);
                    if (rmlRes<0)
                        movementFinished=true; // error
                    else
                    {
                        movementFinished=(rmlRes==1);
                        // Set the current positions:
                        for (int i=0;i<3*dofs;i++)
                            currentPosVelAccel[i]=newPosVelAccel[i];
                        for (int i=0;i<dofs;i++)
                        {
                            CJoint* act=App::currentWorld->sceneObjects->getJointFromHandle(jointHandles[i]);
                            if ( (act!=nullptr)&&(act->getJointType()!=sim_joint_spherical_subtype) )
                            { // might have been destroyed in the mean time
                                if ( (act->getJointMode()==sim_jointmode_force)&&((act->getCumulativeModelProperty()&sim_modelproperty_not_dynamic)==0) )
                                    act->setDynamicMotorPositionControlTargetPosition((float)currentPosVelAccel[0*dofs+i]);
                                else
                                    act->setPosition((float)currentPosVelAccel[0*dofs+i]);
                            }
                        }

                        if (movementFinished)
                        {
                            CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                            threadData->usedMovementTime=(float)(((double*)(auxData+1))[0]);
                            luaWrap_lua_pushinteger(L,1);
                            pushDoubleTableOntoStack(L,dofs,newPosVelAccel+0*dofs);
                            pushDoubleTableOntoStack(L,dofs,newPosVelAccel+1*dofs);
                            pushDoubleTableOntoStack(L,dofs,newPosVelAccel+2*dofs);
                            luaWrap_lua_pushnumber(L,dt-((double*)(auxData+1))[0]);

                            delete[] currentPosVelAccel;
                            delete[] newPosVelAccel;
                            delete[] maxVelAccelJerk;
                            delete[] targetPosVel;
                            delete[] selection;

                            delete[] jointHandles;
                            delete[] currentVel;
                            delete[] currentAccel;
                            delete[] maxVel;
                            delete[] maxAccel;
                            delete[] maxJerk;
                            delete[] targetPos;
                            delete[] targetVel;
                            simRMLRemove_internal(rmlHandle);
                            LUA_END(5);
                        }

                        if (!movementFinished)
                        {
                            CThreadPool::switchBackToPreviousThread();
                            if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                            {
                                errorString="@yield"; // yield will be triggered at end of this function
                                break;
                            }
                        }
                    }
                }
                simRMLRemove_internal(rmlHandle);

                delete[] currentPosVelAccel;
                delete[] newPosVelAccel;
                delete[] maxVelAccelJerk;
                delete[] targetPosVel;
                delete[] selection;
            }

            delete[] jointHandles;
            delete[] currentVel;
            delete[] currentAccel;
            delete[] maxVel;
            delete[] maxAccel;
            delete[] maxJerk;
            delete[] targetPos;
            delete[] targetVel;
        }
        else
            errorString=SIM_ERROR_INVALID_FIRST_ARGUMENT;
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal); // error
    LUA_END(1);
}

int _simRMLMoveToPosition(luaWrap_lua_State* L)
{ // DEPRECATED on 08.09.2020
    TRACE_LUA_API;
    LUA_START_NO_CSIDE_ERROR("sim.rmlMoveToPosition");

    int retVal=-1;
    if (!VThread::isCurrentThreadTheMainSimulationThread())
    {
        if (checkInputArguments(L,nullptr,lua_arg_number,0,lua_arg_number,0,lua_arg_number,0))
        { // the 3 first types are ok!
            int objectHandle=luaToInt(L,1);
            int relativeToObjectHandle=luaToInt(L,2);
            int flags=luaToInt(L,3);
            CSceneObject* it=App::currentWorld->sceneObjects->getObjectFromHandle(objectHandle);
            CSceneObject* relativeIt=nullptr;
            bool relParentError=false;
            if (relativeToObjectHandle!=-1)
            {
                if (relativeToObjectHandle==sim_handle_parent)
                    relativeIt=it->getParent();
                else
                {
                    relativeIt=App::currentWorld->sceneObjects->getObjectFromHandle(relativeToObjectHandle);
                    relParentError=(relativeIt==nullptr);
                }
            }
            if ( (it==nullptr)||((relativeIt==nullptr)&&relParentError) )
                errorString=SIM_ERROR_OBJECT_INEXISTANT;
            else
            {
                C7Vector startPose(it->getCumulativeTransformation());
                C7Vector tr;
                tr.setIdentity();
                if (relativeIt!=nullptr)
                    tr=relativeIt->getCumulativeTransformation();
                C7Vector trInv(tr.getInverse());
                startPose=trInv*startPose;
                C7Vector goalPose(startPose); // if we specify nil for the goal pos/quat we use the same as start

                double currentVel[4]={0.0,0.0,0.0,0.0};
                double currentAccel[4]={0.0,0.0,0.0,0.0};
                double maxVel[4];
                double maxAccel[4];
                double maxJerk[4];
                double targetVel[4]={0.0,0.0,0.0,0.0};
                bool argError=false;

                // currentVel
                int res=checkOneGeneralInputArgument(L,4,lua_arg_number,4,false,true,&errorString);
                if ((!argError)&&(res<1))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,4,4,currentVel);
                }

                // currentAccel
                res=checkOneGeneralInputArgument(L,5,lua_arg_number,4,false,true,&errorString);
                if ((!argError)&&(res<1))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,5,4,currentAccel);
                }

                // maxVel
                res=checkOneGeneralInputArgument(L,6,lua_arg_number,4,false,false,&errorString);
                if ((!argError)&&(res<2))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,6,4,maxVel);
                }

                // maxAccel
                res=checkOneGeneralInputArgument(L,7,lua_arg_number,4,false,false,&errorString);
                if ((!argError)&&(res<2))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,7,4,maxAccel);
                }

                // maxJerk
                res=checkOneGeneralInputArgument(L,8,lua_arg_number,4,false,false,&errorString);
                if ((!argError)&&(res<2))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,8,4,maxJerk);
                }

                // targetPos
                res=checkOneGeneralInputArgument(L,9,lua_arg_number,3,false,true,&errorString);
                if ((!argError)&&(res<1))
                    argError=true;
                else
                {
                    if (res==2)
                    {
                        float dummy[3];
                        getFloatsFromTable(L,9,3,dummy);
                        goalPose.X.set(dummy);
                    }
                }

                // targetQuat
                res=checkOneGeneralInputArgument(L,10,lua_arg_number,4,false,true,&errorString);
                if ((!argError)&&(res<1))
                    argError=true;
                else
                {
                    if (res==2)
                    {
                        float dummy[4];
                        getFloatsFromTable(L,10,4,dummy);
                        goalPose.Q(0)=dummy[3];
                        goalPose.Q(1)=dummy[0];
                        goalPose.Q(2)=dummy[1];
                        goalPose.Q(3)=dummy[2];
                    }
                }

                // targetVel
                res=checkOneGeneralInputArgument(L,11,lua_arg_number,4,false,true,&errorString);
                if ((!argError)&&(res<1))
                    argError=true;
                else
                {
                    if (res==2)
                        getDoublesFromTable(L,11,4,targetVel);
                }

                if (!argError)
                {
                    float matrStart[12];
                    float matrGoal[12];
                    float axis[3];
                    float angle;
                    float quat[4];
                    quat[0]=startPose.Q(1);
                    quat[1]=startPose.Q(2);
                    quat[2]=startPose.Q(3);
                    quat[3]=startPose.Q(0);
                    simBuildMatrixQ_internal(startPose.X.data,quat,matrStart);
                    quat[0]=goalPose.Q(1);
                    quat[1]=goalPose.Q(2);
                    quat[2]=goalPose.Q(3);
                    quat[3]=goalPose.Q(0);
                    simBuildMatrixQ_internal(goalPose.X.data,quat,matrGoal);
                    simGetRotationAxis_internal(matrStart,matrGoal,axis,&angle);
                    unsigned char auxData[9];
                    auxData[0]=1;
                    double currentPosVelAccel[3*4];
                    currentPosVelAccel[0+0]=(double)startPose.X(0);
                    currentPosVelAccel[0+1]=(double)startPose.X(1);
                    currentPosVelAccel[0+2]=(double)startPose.X(2);
                    currentPosVelAccel[0+3]=0.0;
                    for (int i=0;i<4;i++)
                        currentPosVelAccel[4+i]=currentVel[i];
                    for (int i=0;i<4;i++)
                        currentPosVelAccel[8+i]=currentAccel[i];

                    double maxVelAccelJerk[3*4];
                    for (int i=0;i<4;i++)
                        maxVelAccelJerk[0+i]=maxVel[i];
                    for (int i=0;i<4;i++)
                        maxVelAccelJerk[4+i]=maxAccel[i];
                    for (int i=0;i<4;i++)
                        maxVelAccelJerk[8+i]=maxJerk[i];

                    unsigned char selection[4]={1,1,1,1};

                    double targetPosVel[2*4];
                    targetPosVel[0+0]=(double)goalPose.X(0);
                    targetPosVel[0+1]=(double)goalPose.X(1);
                    targetPosVel[0+2]=(double)goalPose.X(2);
                    targetPosVel[0+3]=(double)angle;
                    for (int i=0;i<4;i++)
                        targetPosVel[4+i]=targetVel[i];

                    double newPosVelAccel[3*4];
                    bool movementFinished=false;


                    int rmlHandle=simRMLPos_internal(4,0.0001,flags,currentPosVelAccel,maxVelAccelJerk,selection,targetPosVel,nullptr);
                    while (!movementFinished)
                    {
                        double dt=double(App::currentWorld->simulation->getSimulationTimeStep_speedModified_us())/1000000.0;
                        int rmlRes=simRMLStep_internal(rmlHandle,dt,newPosVelAccel,auxData,nullptr);
                        it=App::currentWorld->sceneObjects->getObjectFromHandle(objectHandle);
                        if ((rmlRes<0)||(it==nullptr))
                            movementFinished=true; // error
                        else
                        {
                            movementFinished=(rmlRes==1);
                            // Set the current position/orientation:
                            for (int i=0;i<3*4;i++)
                                currentPosVelAccel[i]=newPosVelAccel[i];
                            C7Vector currentPose;
                            currentPose.X(0)=(float)currentPosVelAccel[0];
                            currentPose.X(1)=(float)currentPosVelAccel[1];
                            currentPose.X(2)=(float)currentPosVelAccel[2];
                            float matrOut[12];
                            float axisPos[3]={0.0f,0.0f,0.0f};
                            simRotateAroundAxis_internal(matrStart,axis,axisPos,(float)currentPosVelAccel[3],matrOut);
                            simGetQuaternionFromMatrix_internal(matrOut,quat);
                            currentPose.Q(0)=quat[3];
                            currentPose.Q(1)=quat[0];
                            currentPose.Q(2)=quat[1];
                            currentPose.Q(3)=quat[2];
                            currentPose=tr*currentPose;
                            it->setAbsoluteTransformation(currentPose);

                            if (movementFinished)
                            {
                                CVThreadData* threadData=CThreadPool::getCurrentThreadData();
                                threadData->usedMovementTime=(float)(((double*)(auxData+1))[0]);
                                luaWrap_lua_pushinteger(L,1);
                                pushDoubleTableOntoStack(L,3,newPosVelAccel);
                                pushFloatTableOntoStack(L,4,quat);
                                pushDoubleTableOntoStack(L,4,newPosVelAccel+4);
                                pushDoubleTableOntoStack(L,4,newPosVelAccel+8);
                                luaWrap_lua_pushnumber(L,dt-((double*)(auxData+1))[0]);
                                simRMLRemove_internal(rmlHandle);
                                LUA_END(6);
                            }

                            if (!movementFinished)
                            {
                                CThreadPool::switchBackToPreviousThread();
                                if (CThreadPool::getSimulationStopRequestedAndActivated()||(!isObjectAssociatedWithThisThreadedChildScriptValid(L)))
                                {
                                    errorString="@yield"; // yield will be triggered at end of this function
                                    break;
                                }
                            }
                        }
                    }
                    simRMLRemove_internal(rmlHandle);
                }
            }
        }
    }
    else
        errorString=SIM_ERROR_CAN_ONLY_BE_CALLED_FROM_A_THREAD;

    LUA_RAISE_ERROR_OR_YIELD_IF_NEEDED(); // we might never return from this!
    luaWrap_lua_pushinteger(L,retVal); // error
    LUA_END(1);
}
*/
