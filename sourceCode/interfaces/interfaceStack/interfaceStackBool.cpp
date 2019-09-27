
#include "interfaceStackBool.h"

CInterfaceStackBool::CInterfaceStackBool(bool theValue)
{
    _objectType=STACK_OBJECT_BOOL;
    _value=theValue;
}

CInterfaceStackBool::~CInterfaceStackBool()
{
}

bool CInterfaceStackBool::getValue() const
{
    return(_value);
}

void CInterfaceStackBool::setValue(bool theValue)
{
    _value=theValue;
}

CInterfaceStackObject* CInterfaceStackBool::copyYourself() const
{
    CInterfaceStackBool* retVal=new CInterfaceStackBool(_value);
    return(retVal);
}

void CInterfaceStackBool::printContent(int spaces) const
{
    for (int i=0;i<spaces;i++)
        printf(" ");
    if (_value)
        printf("BOOL: true\n");
    else
        printf("BOOL: false\n");
}

std::string CInterfaceStackBool::getObjectData() const
{
    std::string retVal("a");
    if (_value)
        retVal[0]=1;
    else
        retVal[0]=0;
    return(retVal);
}

unsigned int CInterfaceStackBool::createFromData(const char* data)
{
    _value=(data[0]!=0);
    return(1);
}

bool CInterfaceStackBool::checkCreateFromData(const char* data,unsigned int& w,unsigned int l)
{
    if (l<1)
        return(false);
    w=1;
    return(true);
}
