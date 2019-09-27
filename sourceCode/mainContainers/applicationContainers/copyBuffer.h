
#pragma once

#include "vrepMainHeader.h"
#include "3DObject.h"
#include "regCollection.h"
#include "regCollision.h"
#include "regDist.h"
#include "ikGroup.h"
#include "constraintSolverObject.h"
#include "pathPlanningTask_old.h"
#include "motionPlanningTask_old.h"
#include "buttonBlock.h"
#include "luaScriptObject.h"
#include "constraintSolverObject.h"
#include "dynMaterialObject.h"
#include "xmlSer.h"

class CCopyBuffer  
{
public:
    CCopyBuffer();
    virtual ~CCopyBuffer();

    void clearBuffer();
    int pasteBuffer(bool intoLockedScene);
    bool isBufferEmpty();
    void copyCurrentSelection(std::vector<int>* sel,bool fromLockedScene);
    void serializeCurrentSelection(CSer& ar,std::vector<int>* sel,C7Vector& modelTr,C3Vector& modelBBSize,float modelNonDefaultTranslationStepSize);
    bool isCopyForPasting();

    void memorizeBuffer();
    void restoreBuffer();
    void clearMemorizedBuffer();

    void _backupBuffers_temp();
    void _restoreBuffers_temp();
private:

    void _announceObjectWillBeErased(int objectID);
    void _announceGroupWillBeErased(int groupID);
    void _announceIkGroupWillBeErased(int ikGroupID);
    void _announceGcsObjectWillBeErased(int gcsObjectID);
    void _announceCollisionWillBeErased(int collisionID);
    void _announceDistanceWillBeErased(int distanceID);
    void _announce2DElementWillBeErased(int elementID);

    void _eraseObjectInBuffer(int objectID);
    void _eraseGcsObjectInBuffer(int objectID);
    void _erase2DElementInBuffer(int objectID);
    void _eraseLuaScriptInBuffer(int objectID);
    void _erasePathPlanningTaskInBuffer(int objectID);
    void _eraseMotionPlanningTaskInBuffer(int objectID);
    void _eraseCollisionInBuffer(int objectID);
    void _eraseDistanceInBuffer(int objectID);
    void _eraseCollectionInBuffer(int objectID);
    void _eraseIkObjectInBuffer(int objectID);
    void _eraseTextureObjectInBuffer(int objectID); 

    bool _copyIsForPasting;


    bool _bufferIsFromLockedScene;
    std::vector<C3DObject*> objectBuffer;
    std::vector<CRegCollection*> groupBuffer;
    std::vector<CRegCollision*> collisionBuffer;
    std::vector<CRegDist*> distanceBuffer;
    std::vector<CikGroup*> ikGroupBuffer;
    std::vector<CPathPlanningTask*> pathPlanningTaskBuffer;
    std::vector<CMotionPlanningTask*> motionPlanningTaskBuffer;
    std::vector<CButtonBlock*> buttonBlockBuffer;
    std::vector<CLuaScriptObject*> luaScriptBuffer;
    std::vector<CConstraintSolverObject*> constraintSolverBuffer;
    std::vector<CTextureObject*> textureObjectBuffer;

    // Following buffers are used TEMPORARILY when saving a model. They will always be empty before and after serialization
    std::vector<C3DObject*> objectBuffer_tempSer;
    std::vector<CRegCollection*> groupBuffer_tempSer;
    std::vector<CRegCollision*> collisionBuffer_tempSer;
    std::vector<CRegDist*> distanceBuffer_tempSer;
    std::vector<CikGroup*> ikGroupBuffer_tempSer;
    std::vector<CPathPlanningTask*> pathPlanningTaskBuffer_tempSer;
    std::vector<CMotionPlanningTask*> motionPlanningTaskBuffer_tempSer;
    std::vector<CButtonBlock*> buttonBlockBuffer_tempSer;
    std::vector<CLuaScriptObject*> luaScriptBuffer_tempSer;
    std::vector<CConstraintSolverObject*> constraintSolverBuffer_tempSer;
    std::vector<CTextureObject*> textureObjectBuffer_tempSer;



    bool _bufferIsFromLockedScene_memorized;
    std::vector<C3DObject*> objectBuffer_memorized;
    std::vector<CRegCollection*> groupBuffer_memorized;
    std::vector<CRegCollision*> collisionBuffer_memorized;
    std::vector<CRegDist*> distanceBuffer_memorized;
    std::vector<CikGroup*> ikGroupBuffer_memorized;
    std::vector<CPathPlanningTask*> pathPlanningTaskBuffer_memorized;
    std::vector<CMotionPlanningTask*> motionPlanningTaskBuffer_memorized;
    std::vector<CButtonBlock*> buttonBlockBuffer_memorized;
    std::vector<CLuaScriptObject*> luaScriptBuffer_memorized;
    std::vector<CConstraintSolverObject*> constraintSolverBuffer_memorized;
    std::vector<CTextureObject*> textureObjectBuffer_memorized;
};
