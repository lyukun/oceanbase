CREATE OR REPLACE TYPE BODY JSON_OBJECT_T AS
  STATIC function parse(jsn VARCHAR2) return JSON_OBJECT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_PARSE);

  STATIC FUNCTION parse(jsn CLOB) return JSON_OBJECT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_PARSE);

  STATIC FUNCTION parse(jsn BLOB) return JSON_OBJECT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_PARSE);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T(jsn VARCHAR2) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T(jsn CLOB) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T(jsn BLOB) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T(o JSON_ELEMENT_T) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_OBJECT_T(o JSON_OBJECT_T) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_OBJECT_CONSTRUCTOR);

  MEMBER FUNCTION is_Object RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_OBJECT);

  MEMBER FUNCTION is_Array RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_ARRAY);

  MEMBER FUNCTION is_Scalar RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_SCALAR);

  MEMBER FUNCTION is_String RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_STRING);

  MEMBER FUNCTION is_Number RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_NUMBER);

  MEMBER FUNCTION is_Boolean RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_BOOLEAN);

  MEMBER FUNCTION is_True RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_TRUE);

  MEMBER FUNCTION is_False RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_FALSE);

  MEMBER FUNCTION is_Null RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_NULL);

  MEMBER FUNCTION is_Date RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_DATE);

  MEMBER FUNCTION is_Timestamp RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_IS_TIMESTAMP);

  MEMBER FUNCTION to_String RETURN VARCHAR2;
  PRAGMA INTERFACE(c, JSON_TO_STRING);

  MEMBER FUNCTION to_Number    RETURN NUMBER;
  PRAGMA INTERFACE(c, JSON_TO_NUMBER);

  MEMBER FUNCTION to_Date      RETURN DATE;
  PRAGMA INTERFACE(c, JSON_TO_DATE);

  MEMBER FUNCTION to_Timestamp RETURN TIMESTAMP;
  PRAGMA INTERFACE(c, JSON_TO_TIMESTAMP);

  MEMBER FUNCTION to_Boolean   RETURN BOOLEAN;
  PRAGMA INTERFACE(c, JSON_TO_BOOLEAN);

  MEMBER FUNCTION to_Clob      RETURN CLOB;
  PRAGMA INTERFACE(c, JSON_TO_CLOB);

  MEMBER FUNCTION to_Blob      RETURN BLOB;
  PRAGMA INTERFACE(c, JSON_TO_BLOB);

  MEMBER PROCEDURE to_Clob(c IN OUT CLOB);
  PRAGMA INTERFACE(c, JSON_TO_CLOB_PROC);

  MEMBER PROCEDURE to_Blob(c IN OUT BLOB);
  PRAGMA INTERFACE(c, JSON_TO_BLOB_PROC);

  MEMBER FUNCTION get_Size     RETURN NUMBER;
  PRAGMA INTERFACE(c, JSON_GET_SIZE);

  MEMBER FUNCTION get_String(key VARCHAR2) return VARCHAR2;
  PRAGMA INTERFACE(c, JSON_OBJECT_GETSTR);

  MEMBER FUNCTION get_Number(key VARCHAR2) return NUMBER;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_NUMBER);

  MEMBER FUNCTION get_Date(key VARCHAR2) return DATE;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_DATE);

  MEMBER FUNCTION get_Timestamp(key VARCHAR2) return TIMESTAMP;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_TIMESTAMP);

  MEMBER FUNCTION get_Boolean(key VARCHAR2) return BOOLEAN;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_BOOLEAN);

  MEMBER FUNCTION get_Clob(key VARCHAR2) return CLOB;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_CLOB);

  MEMBER FUNCTION get_Blob(key VARCHAR2) return BLOB;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_BLOB);

  MEMBER FUNCTION get_Object(key VARCHAR2) return JSON_OBJECT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_OBJECT);

  MEMBER FUNCTION get(key VARCHAR2) return JSON_ELEMENT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_ELEMENT);

  MEMBER PROCEDURE get_Clob(key VARCHAR2, c IN OUT CLOB);
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_CLOB_PROC);

  MEMBER PROCEDURE get_Blob(key VARCHAR2, c IN OUT BLOB);
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_BLOB_PROC);

  MEMBER PROCEDURE put(key VARCHAR2, value BOOLEAN);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_BOOL);

  MEMBER PROCEDURE put(key VARCHAR2, value JSON_OBJECT_T);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT);

  MEMBER PROCEDURE put(key VARCHAR2, value JSON_ELEMENT_T);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT);

  MEMBER PROCEDURE put(key VARCHAR2, value VARCHAR2);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_VARCHAR);

  MEMBER PROCEDURE put(key VARCHAR2, value NUMBER);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT);

  MEMBER PROCEDURE put(key VARCHAR2, value DATE);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT);

  MEMBER PROCEDURE put(key VARCHAR2, value TIMESTAMP);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT);

  MEMBER PROCEDURE put(key VARCHAR2, value BLOB);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_BLOB);

  MEMBER PROCEDURE put(key VARCHAR2, value CLOB);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_CLOB);

  MEMBER PROCEDURE put(key VARCHAR2, value JSON);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_JSON);

  MEMBER PROCEDURE put_Null(key VARCHAR2);
  PRAGMA INTERFACE(c, JSON_OBJECT_PUT_NULL);

  MEMBER PROCEDURE remove(key VARCHAR2);
  PRAGMA INTERFACE(c, JSON_OBJECT_REMOVE);

  MEMBER PROCEDURE rename_Key(keyOld VARCHAR2, keyNew VARCHAR2);
  PRAGMA INTERFACE(c, JSON_OBJECT_RENAME_KEY);

  MEMBER PROCEDURE on_Error(val NUMBER);
  PRAGMA INTERFACE(c, JSON_OBJECT_ON_ERROR);

  MEMBER FUNCTION has(key VARCHAR2) return BOOLEAN;
  PRAGMA INTERFACE(c, JSON_OBJECT_HAS);

  MEMBER FUNCTION get_Type(key VARCHAR2) return VARCHAR2;
  PRAGMA INTERFACE(c, JSON_OBJECT_GET_TYPE);

  MEMBER FUNCTION clone RETURN JSON_OBJECT_T;
  PRAGMA INTERFACE(c, JSON_OBJECT_CLONE);
END;
//