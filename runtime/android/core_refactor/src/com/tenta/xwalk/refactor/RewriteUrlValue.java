package com.tenta.xwalk.refactor;

public class RewriteUrlValue {
    private String url;
    private int transitionType;
    private boolean isMainFrame;
    
    private void nativeInit(boolean isMainFrame, String url, int trType) {
        this.isMainFrame = isMainFrame;
        this.url = url;
        this.transitionType = trType;
    }
    
    public boolean isMainFrame() {
        return this.isMainFrame;
    }
    public String getUrl() {
        return url;
    }
    
    public void setUrl(String url) {
        this.url = url;
    }
    
    public int getTransitionType() {
        return transitionType;
    }
    
    public void setTransitionType(int transitionType) {
        this.transitionType = transitionType;
    }
    
    
}
